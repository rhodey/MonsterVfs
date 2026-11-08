"""Compare observed SQLite snapshots with acknowledged history outside damaged files."""
from __future__ import annotations
from dataclasses import dataclass
import json
import re

ROWS = 300
COMMITS = 60
ROW_BYTES = 1500
ROW, DONE = 100, 101


@dataclass(frozen=True)
class History:
    snapshots: tuple[tuple[bytes, ...], ...]
    row_bytes: int

    @property
    def rows(self):
        return len(self.snapshots[0])

    @property
    def acknowledged(self):
        return len(self.snapshots) - 1


@dataclass(frozen=True)
class Verdict:
    kind: str
    failed: bool
    reason: str = ''
    fields: dict | None = None


def _require(condition, reason):
    if not condition:
        raise ValueError(reason)


def _integer(value, low=0, high=(1 << 63) - 1):
    return type(value) is int and low <= value <= high


def _keys(value, keys):
    _require(type(value) is dict and set(value) == set(keys), 'missing or unexpected fields')


def _object(pairs):
    result = {}
    for key, value in pairs:
        _require(key not in result, 'duplicate JSON key')
        result[key] = value
    return result


def _constant(value):
    raise ValueError('nonfinite JSON number')


def _decode(text):
    return json.loads(text, object_pairs_hook=_object, parse_constant=_constant)


def _bytes(value, length):
    _require(type(value) is str and len(value) == length * 2
             and re.fullmatch('[0-9a-f]*', value) is not None, 'invalid hex bytes')
    return bytes.fromhex(value)


def load_history(text):
    """Require the entire successful preparation, not a partial acknowledgement file."""
    lines = text.splitlines()
    _require(len(lines) == COMMITS + 2, 'missing or extra history records')
    header = _decode(lines[0])
    expected = dict(format=1, rows=ROWS, commits=COMMITS, row_bytes=ROW_BYTES)
    _keys(header, expected)
    _require(all(type(header[key]) is int and header[key] == value
                 for key, value in expected.items()), 'unexpected history configuration')
    values = [None] * ROWS
    snapshots = []
    for commit, line in enumerate(lines[1:]):
        record = _decode(line)
        _keys(record, ['commit', 'rows', 'ack'])
        _require(type(record['commit']) is int and record['commit'] == commit
                 and record['ack'] is True, 'missing or out-of-order commit acknowledgement')
        changes = record['rows']
        _require(type(changes) is list and len(changes) == (ROWS if commit == 0 else 3),
                 'incomplete intended transaction')
        seen = set()
        for index, change in enumerate(changes):
            _require(type(change) is list and len(change) == 2, 'invalid history row')
            row, encoded = change
            _require(_integer(row, 1, ROWS) and row not in seen, 'duplicate or invalid history ID')
            if commit == 0:
                _require(row == index + 1, 'initial IDs are not ordered')
            seen.add(row)
            values[row - 1] = _bytes(encoded, ROW_BYTES)
        if commit == 0:
            _require(len(seen) == ROWS, 'incomplete initial state')
        snapshots.append(tuple(values))
    return History(tuple(snapshots), ROW_BYTES)


def _error(code):
    return _integer(code, 1, (1 << 31) - 1) and (code & 255) not in (0, ROW, DONE)


def _scan_code(code, count, bound):
    _require(type(code) is int and (code == DONE or _error(code)
             or (code == ROW and count == bound + 1)), 'invalid scan termination')


def _snapshot(value, history):
    """Validate the observer protocol; SQLite-returned data is checked separately."""
    _require(type(value) is dict and 'open' in value, 'missing open result')
    if value['open'] != 0:
        _keys(value, ['open'])
        _require(_error(value['open']), 'invalid open error')
        return value
    _require(type(value['open']) is int and 'begin' in value, 'missing transaction result')
    if value['begin'] != 0:
        _keys(value, ['open', 'begin'])
        _require(_error(value['begin']), 'invalid transaction error')
        return value
    _require(type(value['begin']) is int, 'invalid transaction result')
    _keys(value, ['open', 'begin', 'rows', 'rows_code', 'ledger', 'ledger_code', 'end', 'zero', 'cut'])
    _require(type(value['rows']) is list and len(value['rows']) <= history.rows + 1,
             'invalid row scan size')
    for row in value['rows']:
        _require(type(row) is list and len(row) == 5, 'invalid observed row')
        id_type, identity, data_type, length, encoded = row
        _require(_integer(id_type, 1, 5) and _integer(identity, -(1 << 63))
                 and _integer(data_type, 1, 5) and _integer(length, 0, (1 << 31) - 1),
                 'invalid observed value type/length')
        _bytes(encoded, min(length, history.row_bytes + 1))
    _require(type(value['ledger']) is list and len(value['ledger']) <= history.acknowledged + 1,
             'invalid ledger scan size')
    for item in value['ledger']:
        _require(type(item) is list and len(item) == 2 and _integer(item[0], 1, 5)
                 and _integer(item[1], -(1 << 63)), 'invalid observed ledger row')
    _scan_code(value['rows_code'], len(value['rows']), history.rows)
    _scan_code(value['ledger_code'], len(value['ledger']), history.acknowledged)
    _require(type(value['end']) is int and (value['end'] == 0 or _error(value['end'])),
             'invalid transaction end result')
    _require(_integer(value['zero']) and _integer(value['cut']), 'invalid status counters')
    return value


def _candidates(snapshot, history):
    candidates = set(range(history.acknowledged + 1))
    if 'rows' not in snapshot:
        return candidates
    # Ordered scans must return a prefix of the expected IDs, even before an error.
    rows = snapshot['rows']
    if len(rows) > history.rows or (snapshot['rows_code'] == DONE and len(rows) != history.rows):
        return set()
    for index, (id_type, identity, data_type, length, encoded) in enumerate(rows):
        if id_type != 1 or identity != index + 1 or data_type != 3 or length != history.row_bytes:
            return set()
        data = bytes.fromhex(encoded)
        candidates = {c for c in candidates if history.snapshots[c][index] == data}
    ledger = snapshot['ledger']
    if any(kind != 1 or identity != index + 1 for index, (kind, identity) in enumerate(ledger)):
        return set()
    count = len(ledger)
    if snapshot['ledger_code'] == DONE:
        candidates.intersection_update([count])
    else:
        candidates = {c for c in candidates if c >= count}
    return candidates


def evaluate(code, output, diagnostic, *, history, native=False, allowed_commits=None):
    if code is None:
        return Verdict('timeout', True, 'child did not finish')
    if code != 0:
        return Verdict('process-error', True, f'child exit {code}')
    if 'panicked' in diagnostic:
        return Verdict('process-error', True, 'child reported a panic')
    if not isinstance(history, History):
        return Verdict('invalid-result', True, 'expected history unavailable')
    try:
        if allowed_commits is None:
            common = set(range(history.acknowledged + 1))
        else:
            _require(type(allowed_commits) in (set, frozenset) and allowed_commits
                     and all(_integer(commit, 0, history.acknowledged)
                             for commit in allowed_commits), 'invalid allowed commits')
            common = set(allowed_commits)
        result = _decode(output)
        _keys(result, ['passes'])
        passes = result['passes']
        _require(type(passes) is list and 1 <= len(passes) <= 2, 'missing reopen results')
        passes = [_snapshot(item, history) for item in passes]
        _require(len(passes) == (1 if passes[0]['open'] else 2), 'incomplete reopen results')
    except (ValueError, TypeError, RecursionError) as error:
        return Verdict('invalid-result', True, str(error))
    if passes[0]['open']:
        return Verdict('open-refused', False, fields={'codes': [passes[0]['open']]})
    complete = True
    fields = {'rows': 0, 'zero': 0, 'cut': 0, 'codes': [], 'commit': None, 'lost_commits': None}
    for snapshot in passes:
        common.intersection_update(_candidates(snapshot, history))
        if 'rows' not in snapshot:
            complete = False
            fields['codes'].append(snapshot['open'] or snapshot['begin'])
            continue
        fields['rows'] += len(snapshot['rows'])
        fields['zero'] = max(fields['zero'], snapshot['zero'])
        fields['cut'] = max(fields['cut'], snapshot['cut'])
        for name in ['rows_code', 'ledger_code', 'end']:
            value = snapshot[name]
            if value not in (0, DONE):
                complete = False
                if value != ROW:
                    fields['codes'].append(value)
    if not common:
        return Verdict('wrong-data', not native,
                       'observations do not match one allowed transaction prefix', fields)
    if not complete:
        return Verdict('read-error', False, 'only partial history verification was possible', fields)
    _require(len(common) == 1, 'complete snapshots do not identify one prefix')
    commit = common.pop()
    fields.update(commit=commit, lost_commits=history.acknowledged - commit)
    return Verdict('exact-recovery' if commit == history.acknowledged else 'prefix-loss', False, fields=fields)
