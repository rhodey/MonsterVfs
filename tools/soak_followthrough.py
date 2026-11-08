"""Exercise new work after a completely verified recovery, using external intent."""
from __future__ import annotations

import json
from pathlib import Path

from soak_verdict import (History, Verdict, evaluate, _decode, _error, _integer,
                          _keys, _require)


def plan(history, commit):
    """Extend the recovered prefix with three externally specified row replacements."""
    _require(_integer(commit, 0, history.acknowledged), 'invalid recovered prefix')
    rows = []
    values = list(history.snapshots[commit])
    for part, identity in enumerate((1, 150, 300)):
        tag = f'followup:{commit + 1}:{identity}:'.encode('ascii')
        value = tag + bytes([ord('X') + part]) * (history.row_bytes - len(tag))
        values[identity - 1] = value
        rows.append([identity, value.hex()])
    intent = {'base_commit': commit, 'commit': commit + 1, 'rows': rows}
    extended = History(history.snapshots[:commit + 1] + (tuple(values),), history.row_bytes)
    return intent, extended


def operation(outcome, checkpoint=False):
    """Parse an operation result without interpreting a SQLite error as success."""
    code, output, diagnostic = outcome
    if code is None:
        return None, Verdict('timeout', True, 'follow-through child did not finish')
    if code != 0 or 'panicked' in diagnostic:
        return None, Verdict('process-error', True, f'follow-through child exit {code}')
    try:
        result = _decode(output)
        _require(type(result) is dict, 'operation result must be an object')
        stage = result.get('stage')
        stages = ('open', 'configure', 'checkpoint') if checkpoint else (
            'open', 'configure', 'begin', 'update', 'ledger', 'commit')
        _require(stage in stages, 'unexpected operation stage')
        fields = ['stage', 'code']
        if checkpoint and stage == 'checkpoint':
            fields += ['log', 'checkpointed']
        _keys(result, fields)
        rc = result['code']
        _require(type(rc) is int and (rc == 0 or _error(rc)), 'invalid operation code')
        _require(rc != 0 or stage == stages[-1], 'operation stopped before completion')
        if checkpoint and stage == 'checkpoint':
            for name in ('log', 'checkpointed'):
                _require(_integer(result[name], -1, (1 << 31) - 1), 'invalid checkpoint count')
    except (ValueError, TypeError, RecursionError) as error:
        return None, Verdict('invalid-result', True, str(error))
    return result, None


def run(invoke, database, history, commit, native=False):
    """Write, inspect, explicitly checkpoint, and inspect again without new damage."""
    database = Path(database)
    intent, extended = plan(history, commit)
    payload = database.parent / 'followup.bin'
    payload.write_bytes(b''.join(bytes.fromhex(row[1]) for row in intent['rows']))
    (database.parent / 'followup.json').write_text(json.dumps(intent) + '\n')
    evidence = {'intent': intent}
    fields = {'write_acknowledged': False, 'codes': []}

    def call(name, *args):
        outcome = invoke(*args)
        evidence[name] = dict(zip(('code', 'stdout', 'stderr'), outcome))
        return outcome

    def inspect(name, allowed):
        verdict = evaluate(*call(name, 'inspect', str(database), str(extended.acknowledged)),
                           history=extended, allowed_commits=allowed, native=native)
        evidence[name]['verdict'] = verdict.kind
        evidence[name]['reason'] = verdict.reason
        fields[name] = verdict.kind
        if verdict.fields:
            fields['codes'].extend(verdict.fields['codes'])
        return verdict

    def result(kind, failed=False, reason=''):
        return Verdict(kind, failed, reason, fields.copy()), evidence

    def observation_failure(verdict):
        if verdict.failed or verdict.kind == 'wrong-data':
            return result(verdict.kind, verdict.failed, verdict.reason)
        if verdict.kind not in ('exact-recovery', 'prefix-loss'):
            return result('reopen-error', reason=verdict.reason)
        return None

    write, failure = operation(call('write', 'write', str(database),
                                    str(intent['commit']), str(payload)))
    if failure:
        return result(failure.kind, failure.failed, failure.reason)
    fields.update(write_stage=write['stage'], write_code=write['code'],
                  write_acknowledged=write['code'] == 0)
    if write['code']:
        fields['codes'].append(write['code'])
    allowed = ({intent['commit']} if fields['write_acknowledged'] else
               {commit, intent['commit']} if write['stage'] == 'commit' else {commit})
    after_write = inspect('after_write', allowed)
    # Contradictory data wins over an operation error. A partial scan never
    # authorizes a checkpoint that could discard the evidence of that error.
    failure = observation_failure(after_write)
    if failure and (after_write.failed or after_write.kind == 'wrong-data'):
        return failure
    if write['code']:
        return result('write-error', reason=f"{write['stage']} returned {write['code']}")
    if failure:
        return failure

    checkpoint, failure = operation(call('checkpoint', 'checkpoint', str(database)), checkpoint=True)
    if failure:
        return result(failure.kind, failure.failed, failure.reason)
    fields.update(checkpoint_stage=checkpoint['stage'], checkpoint_code=checkpoint['code'])
    if checkpoint['code']:
        fields['codes'].append(checkpoint['code'])
    complete = (checkpoint['stage'] == 'checkpoint' and checkpoint['code'] == 0
                and checkpoint['log'] == checkpoint['checkpointed'] == 0)
    # Even a failed checkpoint must not silently remove an acknowledged write.
    after_checkpoint = inspect('after_checkpoint', {intent['commit']})
    failure = observation_failure(after_checkpoint)
    if failure and (after_checkpoint.failed or after_checkpoint.kind == 'wrong-data'):
        return failure
    if not complete:
        return result('checkpoint-error', reason='TRUNCATE checkpoint did not complete')
    if failure:
        return failure
    return result('usable')
