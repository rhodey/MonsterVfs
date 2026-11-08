#!/usr/bin/env python3
"""Exercise post-recovery writes and checkpoint outcomes through the real C host."""
from __future__ import annotations

from contextlib import closing
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

from soak_oracle import (TIMEOUT, actual_snapshot, compare_observation, copy_database,
                         mutate, require)
from soak_followthrough import run
from soak_verdict import History, load_history


IDS = (1, 150, 300)


class Host:
    def __init__(self, binary, library):
        self.binary, self.library = binary, library
        self.calls = []
        self.payload = None
        self.wal_after_write = None
        self.wal_before_checkpoint = None

    def invoke(self, *args):
        if args[0] == 'write':
            self.payload = Path(args[3]).read_bytes()
            require(len(self.payload) == 4500, 'follow-up payload is not exactly three rows')
            intent = Path(args[1]).parent / 'followup.json'
            require(intent.is_file(), 'external intent was not written before the mutation')
        if args[0] == 'checkpoint':
            wal = Path(str(args[1]) + '-wal')
            self.wal_before_checkpoint = wal.stat().st_size if wal.exists() else 0
        try:
            result = subprocess.run([self.binary, self.library, 'unix', *map(str, args)],
                                    capture_output=True, text=True, errors='replace',
                                    timeout=TIMEOUT)
        except subprocess.TimeoutExpired as error:
            raise AssertionError(f'{args[0]} timed out after {TIMEOUT}s') from error
        outcome = (result.returncode, result.stdout, result.stderr)
        if args[0] == 'write':
            wal = Path(str(args[1]) + '-wal')
            self.wal_after_write = wal.stat().st_size if wal.exists() else 0
        self.calls.append((args, outcome))
        return outcome


def observation(snapshot, commit):
    return dict(open=0, begin=0,
                rows=[[1, index, 3, len(value), value.hex()]
                      for index, value in enumerate(snapshot, 1)],
                rows_code=101, ledger=[[1, index] for index in range(1, commit + 1)],
                ledger_code=101, end=0, zero=0, cut=0)


def parsed(outcome, context):
    code, output, diagnostic = outcome
    require(code == 0, f'{context}: child exit {code}: {diagnostic}')
    try:
        return json.loads(output)
    except (ValueError, TypeError) as error:
        raise AssertionError(f'{context}: malformed child observation: {output[-1000:]}') from error


def check_inspection(outcome, expected, context):
    result = parsed(outcome, context)
    require(type(result) is dict and set(result) == {'passes'} and len(result['passes']) == 2,
            f'{context}: expected two completed reopens')
    for index, snapshot in enumerate(result['passes'], 1):
        compare_observation(snapshot, expected, f'{context} reopen {index}')


def run_fixture(binary, library, path, history, commit, kind):
    host = Host(binary, library)
    verdict, evidence = run(host.invoke, path, history, commit)
    require(verdict.kind == kind and not verdict.failed,
            f'{path.parent.name}: expected {kind}, got {verdict.kind}: {verdict.reason}')
    require(host.payload is not None, 'no follow-up write was attempted')
    old = history.snapshots[commit]
    new = list(old)
    for part, identity in enumerate(IDS):
        new[identity - 1] = host.payload[part * history.row_bytes:(part + 1) * history.row_bytes]
        require(new[identity - 1] != old[identity - 1], 'follow-up did not change its target row')
    new = tuple(new)
    written = parsed(host.calls[0][1], 'write')
    require(host.calls[0][0][:3] == ('write', str(path), str(commit + 1)),
            'writer did not receive the next commit after the recovered prefix')
    require(evidence['intent']['base_commit'] == commit
            and evidence['intent']['commit'] == commit + 1,
            'external intent does not identify the recovered prefix')
    require(evidence['intent']['rows'] == [[identity, new[identity - 1].hex()] for identity in IDS],
            'external intent differs from the bytes passed to the writer')
    acknowledged = kind != 'write-error'
    require(verdict.fields['write_acknowledged'] is acknowledged,
            'COMMIT acknowledgement was misclassified')
    if acknowledged:
        require(written == {'stage': 'commit', 'code': 0}, 'writer did not acknowledge COMMIT')
        require(host.wal_after_write > 32 and host.wal_before_checkpoint > 32,
                'writer or intervening inspections checkpointed before the explicit request')
        require([args[0] for args, _ in host.calls] == ['write', 'inspect', 'checkpoint', 'inspect'],
                'follow-through did not inspect on both sides of its explicit checkpoint')
        checkpoint = parsed(host.calls[2][1], 'checkpoint')
        require(checkpoint['stage'] == 'checkpoint', 'checkpoint did not reach SQLite')
        if kind == 'usable':
            require(checkpoint == {'stage': 'checkpoint', 'code': 0, 'log': 0, 'checkpointed': 0},
                    'usable result did not complete the TRUNCATE checkpoint')
        else:
            require(checkpoint['code'] & 255 == 5, 'held reader did not produce SQLITE_BUSY')
            require(checkpoint['log'] > checkpoint['checkpointed'] >= 0,
                    'blocked checkpoint did not report uncheckpointed frames')
        expected = observation(new, commit + 1)
        check_inspection(host.calls[1][1], expected, 'before checkpoint')
        check_inspection(host.calls[3][1], expected, 'after checkpoint')
        expected_history = History(history.snapshots[:commit + 1] + (new,), history.row_bytes)
    else:
        require(written['stage'] == 'update' and written['code'] & 255 == 19,
                'trigger did not abort the intended update with SQLITE_CONSTRAINT')
        require([args[0] for args, _ in host.calls] == ['write', 'inspect'],
                'an aborted mutation must be inspected without checkpointing')
        expected = observation(old, commit)
        check_inspection(host.calls[1][1], expected, 'after aborted write')
        expected_history = History(history.snapshots[:commit + 1], history.row_bytes)
    # A separate SQLite connection confirms types and bytes independently of C and its oracle.
    compare_observation(actual_snapshot(path, expected_history, False), expected,
                        f'{path.parent.name} independent SQLite read')
    return expected, expected_history


def regressions(binary, library, work):
    prepared = work / 'prepared.db'
    prepared_result = Host(binary, library).invoke('prepare', str(prepared))
    require(prepared_result[0] == 0, f'prepare failed: {prepared_result[2]}')
    (work / 'history.jsonl').write_text(prepared_result[1], encoding='utf-8')
    history = load_history(prepared_result[1])
    template = work / 'template' / 'db'
    copy_database(prepared, template)

    latest = work / 'latest' / 'db'
    copy_database(template, latest)
    run_fixture(binary, library, latest, history, history.acknowledged, 'usable')

    prefix = work / 'prefix' / 'db'
    copy_database(template, prefix)
    commit = 30
    mutations = [('DELETE FROM t', ()), ('DELETE FROM ledger', ())]
    mutations += [('INSERT INTO t VALUES(?,?)', (index, value.decode('utf-8')))
                  for index, value in enumerate(history.snapshots[commit], 1)]
    mutations += [('INSERT INTO ledger VALUES(?)', (index,)) for index in range(1, commit + 1)]
    mutate(prefix, mutations)
    run_fixture(binary, library, prefix, history, commit, 'usable')

    aborted = work / 'aborted' / 'db'
    copy_database(template, aborted)
    mutate(aborted, [("CREATE TRIGGER abort_followup BEFORE UPDATE ON t WHEN NEW.id=150 "
                      "BEGIN SELECT RAISE(ABORT, 'stop fixture'); END", ())])
    run_fixture(binary, library, aborted, history, history.acknowledged, 'write-error')

    blocked = work / 'blocked-checkpoint' / 'db'
    copy_database(template, blocked)
    with closing(sqlite3.connect(str(blocked), timeout=10)) as reader:
        reader.execute('BEGIN')
        require(reader.execute('SELECT count(*) FROM t').fetchone() == (history.rows,),
                'reader did not pin the old snapshot')
        expected, expected_history = run_fixture(binary, library, blocked, history,
                                                 history.acknowledged, 'checkpoint-error')
        require(reader.execute('SELECT max(i) FROM ledger').fetchone() == (history.acknowledged,),
                'held reader did not retain its original snapshot')
        reader.rollback()
    compare_observation(actual_snapshot(blocked, expected_history, False), expected,
                        'after releasing the checkpoint-blocking reader')
    final = Host(binary, library).invoke('inspect', str(blocked), str(expected_history.acknowledged))
    check_inspection(final, expected, 'fresh C reopens after releasing the reader')
    print('Soak follow-through regressions passed: latest/prefix commits, aborted write, and blocked checkpoint.')


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: soak_followthrough.py SOAK_BINARY EXTENSION')
    with tempfile.TemporaryDirectory(prefix='monstervfs-soak-followthrough-') as temporary:
        regressions(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()),
                    Path(temporary).resolve())


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, ValueError, sqlite3.Error) as error:
        raise SystemExit(f'Soak follow-through regression failed: {error}')
