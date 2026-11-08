#!/usr/bin/env python3
"""Exercise the real soak observer and external oracle with native SQLite fixtures."""
from __future__ import annotations

from contextlib import closing
import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from soak_verdict import evaluate, load_history


TIMEOUT = 120
SQLITE_ROW, SQLITE_DONE, SQLITE_ERROR = 100, 101, 1
TYPES = {b'integer': 1, b'real': 2, b'text': 3, b'blob': 4, b'null': 5}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def invoke(binary, library, mode, path):
    try:
        return subprocess.run([binary, library, 'unix', mode, str(path)],
                              capture_output=True, text=True, errors='replace',
                              timeout=TIMEOUT)
    except subprocess.TimeoutExpired as error:
        diagnostic = error.stderr or b''
        if isinstance(diagnostic, bytes):
            diagnostic = diagnostic.decode('utf-8', errors='replace')
        raise AssertionError(f'{mode} timed out after {TIMEOUT}s\nstderr:\n{diagnostic}') from error


def copy_database(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    wal = Path(str(source) + '-wal')
    if wal.exists():
        shutil.copyfile(wal, str(target) + '-wal')


def mutate(path, operations):
    if not operations:
        return
    with closing(sqlite3.connect(str(path), timeout=10)) as db:
        with db:
            for statement, parameters in operations:
                db.execute(statement, parameters)


def actual_snapshot(path, history, missing_ledger):
    """Read SQL types and exact bytes independently of the C observation format."""
    with closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True, timeout=10)) as db:
        db.text_factory = bytes
        rows = []
        for id_type, identity, value_type, value in db.execute(
                'SELECT typeof(id),id,typeof(v),CAST(v AS BLOB) FROM t ORDER BY id'):
            value = b'' if value is None else value
            rows.append([TYPES[id_type], identity, TYPES[value_type], len(value),
                         value[:history.row_bytes + 1].hex()])
        if missing_ledger:
            require(db.execute("SELECT count(*) FROM sqlite_master WHERE name='ledger'").fetchone()[0] == 0,
                    'fixture still has its supposedly missing ledger')
            ledger, ledger_code = [], SQLITE_ERROR
        else:
            ledger = [[TYPES[kind], identity] for kind, identity in db.execute(
                'SELECT typeof(i),i FROM ledger ORDER BY i')]
            ledger_code = SQLITE_ROW if len(ledger) == history.acknowledged + 1 else SQLITE_DONE
    require(len(rows) <= history.rows + 1, 'fixture exceeds the observer row limit')
    require(len(ledger) <= history.acknowledged + 1, 'fixture exceeds the observer ledger limit')
    return dict(open=0, begin=0, rows=rows,
                rows_code=SQLITE_ROW if len(rows) == history.rows + 1 else SQLITE_DONE,
                ledger=ledger, ledger_code=ledger_code, end=0, zero=0, cut=0)


def compare_observation(observed, expected, name):
    require(type(observed) is dict and set(observed) == set(expected),
            f'{name}: unexpected observation fields')
    for key in expected:
        if observed[key] != expected[key]:
            if key in ('rows', 'ledger') and isinstance(observed[key], list):
                left, right = observed[key], expected[key]
                for index, (actual, wanted) in enumerate(zip(left, right)):
                    require(actual == wanted, f'{name}: {key}[{index}] differs from SQLite data')
                raise AssertionError(f'{name}: {key} count {len(left)}, expected {len(right)}')
            raise AssertionError(f'{name}: {key}={observed[key]!r}, expected {expected[key]!r}')


def check_fixture(binary, library, path, history, name, kind, commit, missing_ledger):
    expected = actual_snapshot(path, history, missing_ledger)
    result = invoke(binary, library, 'check', path)
    try:
        # Native storage makes controlled SQL mutations possible. Verdicts remain
        # strict here: an expected wrong-data fixture must set failed=True.
        verdict = evaluate(result.returncode, result.stdout, result.stderr, history=history)
        require(verdict.kind == kind and verdict.failed == (kind == 'wrong-data'),
                f'{name}: expected {kind}, got {verdict.kind} '
                f'(failed={verdict.failed}, reason={verdict.reason})')
        observed = json.loads(result.stdout)
        require(set(observed) == {'passes'} and len(observed['passes']) == 2,
                f'{name}: observer did not complete two reopens')
        for number, snapshot in enumerate(observed['passes'], 1):
            compare_observation(snapshot, expected, f'{name} reopen {number}')
        if commit is not None:
            require(verdict.fields['commit'] == commit
                    and verdict.fields['lost_commits'] == history.acknowledged - commit,
                    f'{name}: incorrect recovered prefix or lost-commit count')
            values = tuple(bytes.fromhex(row[4]) for row in expected['rows'])
            require(values == history.snapshots[commit],
                    f'{name}: actual SQLite bytes do not match the recorded prefix')
    except (AssertionError, ValueError, TypeError, KeyError) as error:
        raise AssertionError(f'{error}\nchild exit: {result.returncode}\n'
                             f'stdout tail:\n{result.stdout[-1200:]}\n'
                             f'stderr:\n{result.stderr}') from error


def regressions(binary, library, work):
    prepared = work / 'prepared.db'
    result = invoke(binary, library, 'prepare', prepared)
    require(result.returncode == 0,
            f'prepare exited {result.returncode}\nstderr:\n{result.stderr}')
    history_path = work / 'history.jsonl'
    history_path.write_text(result.stdout, encoding='utf-8')
    try:
        history = load_history(history_path.read_text(encoding='utf-8'))
    except (ValueError, TypeError) as error:
        raise AssertionError(f'invalid prepare history: {error}\nstderr:\n{result.stderr}') from error
    require(Path(str(prepared) + '-wal').is_file(), 'prepare did not leave a live WAL')
    template = work / 'template' / 'db'
    copy_database(prepared, template)

    latest = history.acknowledged
    changed = [index for index in range(history.rows)
               if history.snapshots[latest][index] != history.snapshots[latest - 1][index]]
    require(len(changed) >= 2, 'last commit cannot produce a mixed-transaction fixture')
    stale_index = changed[0]
    mixed = list(history.snapshots[latest])
    mixed[stale_index] = history.snapshots[latest - 1][stale_index]
    require(tuple(mixed) not in history.snapshots, 'mixed fixture is an acknowledged prefix')
    stale = ('UPDATE t SET v=? WHERE id=?',
             (mixed[stale_index].decode('utf-8'), stale_index + 1))
    prefix = 30
    replace_prefix = [('DELETE FROM t', ()), ('DELETE FROM ledger', ())]
    replace_prefix += [('INSERT INTO t VALUES(?,?)', (index, value.decode('utf-8')))
                       for index, value in enumerate(history.snapshots[prefix], 1)]
    replace_prefix += [('INSERT INTO ledger VALUES(?)', (commit,))
                       for commit in range(1, prefix + 1)]
    drop_ledger = ('DROP TABLE ledger', ())
    # name, expected verdict, recovered commit, missing ledger, SQL mutations
    fixtures = [
        ('exact', 'exact-recovery', latest, False, []),
        ('prefix-30', 'prefix-loss', prefix, False, replace_prefix),
        ('mixed-transactions', 'wrong-data', None, False, [stale]),
        ('ledger-gap', 'wrong-data', None, False,
         [('DELETE FROM ledger WHERE i=?', (prefix,))]),
        ('ledger-extra', 'wrong-data', None, False,
         [('INSERT INTO ledger VALUES(?)', (latest + 1,))]),
        ('missing-row', 'wrong-data', None, False,
         [('DELETE FROM t WHERE id=150', ())]),
        ('extra-row', 'wrong-data', None, False,
         [('INSERT INTO t VALUES(?,?)', (history.rows + 1, history.snapshots[latest][0].decode('utf-8')))]),
        ('blob-same-bytes', 'wrong-data', None, False,
         [('UPDATE t SET v=CAST(v AS BLOB) WHERE id=150', ())]),
        ('null-value', 'wrong-data', None, False,
         [('UPDATE t SET v=NULL WHERE id=150', ())]),
        ('duplicate-id', 'wrong-data', None, False,
         [('ALTER TABLE t RENAME TO old_t', ()),
          ('CREATE TABLE t(id INTEGER, v TEXT)', ()),
          ('INSERT INTO t SELECT id,v FROM old_t', ()),
          ('DROP TABLE old_t', ()),
          ('UPDATE t SET id=149,v=(SELECT v FROM t WHERE id=149) WHERE id=150', ())]),
        ('missing-ledger', 'read-error', None, True, [drop_ledger]),
        ('missing-ledger-mixed-rows', 'wrong-data', None, True, [stale, drop_ledger]),
    ]
    for name, kind, commit, missing_ledger, operations in fixtures:
        path = work / name / 'db'
        copy_database(template, path)
        mutate(path, operations)
        check_fixture(binary, library, path, history, name, kind, commit, missing_ledger)
    print(f'Soak observer/oracle regressions passed: {len(fixtures)} fixtures, '
          f'{len(fixtures) * 2} reopens.')


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: soak_oracle.py SOAK_BINARY EXTENSION')
    with tempfile.TemporaryDirectory(prefix='monstervfs-soak-oracle-') as temporary:
        regressions(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()),
                    Path(temporary).resolve())


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, sqlite3.Error) as error:
        raise SystemExit(f'Soak observer/oracle regression failed: {error}')
