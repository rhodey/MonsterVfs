"""Recovery evidence must describe one acknowledged transaction prefix."""
import copy
import json
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from soak_verdict import evaluate, load_history

ROWS = 300
COMMITS = 60
ROW_BYTES = 1500
DONE = 101
ROW = 100
IOERR = 266


def payload(commit, row_id):
    unit = f'{commit:03}/{row_id:03}/'.encode('ascii')
    return (unit * (ROW_BYTES // len(unit) + 1))[:ROW_BYTES]


def history_records():
    records = [{'format': 1, 'rows': ROWS, 'commits': COMMITS,
                'row_bytes': ROW_BYTES}]
    for commit in range(COMMITS + 1):
        # The recorded intent, rather than the C workload's row formula, is the oracle.
        ids = range(1, ROWS + 1) if commit == 0 else (1, 2, 3)
        records.append({'commit': commit, 'rows': [
            [row_id, payload(commit, row_id).hex()] for row_id in ids], 'ack': True})
    return records


def history_text(records):
    return '\n'.join(json.dumps(record, separators=(',', ':')) for record in records) + '\n'


RECORDS = history_records()
HISTORY_TEXT = history_text(RECORDS)


def snapshot(commit=COMMITS, **changes):
    result = {
        'open': 0, 'begin': 0,
        'rows': [[1, row_id, 3, ROW_BYTES,
                  payload(commit if row_id <= 3 else 0, row_id).hex()]
                 for row_id in range(1, ROWS + 1)],
        'rows_code': DONE,
        'ledger': [[1, value] for value in range(1, commit + 1)],
        'ledger_code': DONE, 'end': 0, 'zero': 0, 'cut': 0,
    }
    result.update(changes)
    return result


class SoakVerdictTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.history = load_history(HISTORY_TEXT)

    def verdict(self, *passes, native=False, code=0, diagnostic=''):
        return evaluate(code, json.dumps({'passes': passes}), diagnostic,
                        history=self.history, native=native)

    def assert_kind(self, kind, *passes, native=False):
        verdict = self.verdict(*passes, native=native)
        self.assertEqual(verdict.kind, kind, verdict.reason)
        self.assertEqual(verdict.failed,
                         kind in ('invalid-result', 'wrong-data') and not
                         (kind == 'wrong-data' and native), verdict.reason)
        return verdict

    def test_complete_acknowledged_prefixes(self):
        for commit in (0, 20, COMMITS):
            with self.subTest(commit=commit):
                expected = 'exact-recovery' if commit == COMMITS else 'prefix-loss'
                self.assert_kind(expected, snapshot(commit), snapshot(commit))

    def test_mixed_individually_valid_row_versions_fail(self):
        mixed = snapshot()
        mixed['rows'][0][4] = payload(59, 1).hex()
        mixed['rows'][1][4] = payload(58, 2).hex()
        self.assert_kind('wrong-data', mixed, mixed)

    def test_correct_rows_with_a_different_ledger_fail(self):
        mixed = snapshot(20, ledger=snapshot(19)['ledger'])
        self.assert_kind('wrong-data', mixed, mixed)

    def test_wrong_bytes_are_not_excused_by_recovery_counters(self):
        for zero, cut in ((0, 0), (1, 1), (300, 60)):
            with self.subTest(zero=zero, cut=cut):
                wrong = snapshot(zero=zero, cut=cut)
                wrong['rows'][10][4] = (b'\0' * ROW_BYTES).hex()
                self.assert_kind('wrong-data', wrong, wrong)

    def test_native_wrong_data_is_observed_without_failing_control(self):
        wrong = snapshot()
        wrong['rows'][0][4] = payload(59, 1).hex()
        self.assert_kind('wrong-data', wrong, wrong, native=True)

    def test_missing_extra_duplicate_and_unordered_rows_fail(self):
        cases = {}
        cases['missing'] = snapshot()
        cases['missing']['rows'].pop()
        cases['extra'] = snapshot(rows_code=ROW)
        cases['extra']['rows'].append([1, 301, 3, ROW_BYTES, payload(0, 301).hex()])
        cases['duplicate'] = snapshot()
        cases['duplicate']['rows'][1] = cases['duplicate']['rows'][0][:]
        cases['unknown'] = snapshot()
        cases['unknown']['rows'][-1][1] = 301
        cases['unordered'] = snapshot()
        cases['unordered']['rows'][0:2] = reversed(cases['unordered']['rows'][0:2])
        for name, damaged in cases.items():
            with self.subTest(name=name):
                self.assert_kind('wrong-data', damaged, damaged)

    def test_ledger_requires_every_commit_not_just_a_plausible_maximum(self):
        cases = [list(range(1, 30)) + list(range(31, 61)),
                 list(range(1, 30)) + [29] + list(range(31, 61)),
                 list(range(2, 61)), [0] + list(range(2, 61)),
                 [2, 1] + list(range(3, 61))]
        for ids in cases:
            with self.subTest(ids=ids):
                damaged = snapshot(ledger=[[1, value] for value in ids])
                self.assert_kind('wrong-data', damaged, damaged)

    def test_ledger_scan_limit_is_not_a_successful_prefix(self):
        damaged = snapshot(ledger=[[1, value] for value in range(1, 62)],
                           ledger_code=ROW)
        self.assert_kind('wrong-data', damaged, damaged)

    def test_sql_null_and_wrong_storage_types_are_wrong_data(self):
        for column, value in ((0, 3), (2, 1), (2, 2), (2, 4), (2, 5)):
            with self.subTest(column=column, value=value):
                damaged = snapshot()
                damaged['rows'][0][column] = value
                if value == 5:
                    damaged['rows'][0][3:] = [0, '']
                self.assert_kind('wrong-data', damaged, damaged)
        damaged = snapshot()
        damaged['ledger'][0][0] = 3
        self.assert_kind('wrong-data', damaged, damaged)

    def test_exact_payload_length_and_last_byte_are_checked(self):
        for data in (payload(60, 1)[:-1], payload(60, 1) + b'x',
                     payload(60, 1)[:-1] + b'x'):
            with self.subTest(length=len(data)):
                damaged = snapshot()
                damaged['rows'][0][3:] = [len(data), data.hex()]
                self.assert_kind('wrong-data', damaged, damaged)

    def test_capped_oversized_value_is_wrong_data_not_bad_transport(self):
        damaged = snapshot()
        damaged['rows'][0][3:] = [2000, (b'x' * (ROW_BYTES + 1)).hex()]
        self.assert_kind('wrong-data', damaged, damaged)

    def test_sql_integer_ids_outside_the_history_are_wrong_data(self):
        for row_id in (-(2**63), -1, 0, 2**63 - 1):
            with self.subTest(row_id=row_id):
                damaged = snapshot()
                damaged['rows'][0][1] = row_id
                self.assert_kind('wrong-data', damaged, damaged)

    def test_partial_compatible_reads_remain_unverified(self):
        for rows_count, ledger_count in ((0, 0), (2, 0), (3, 10), (300, 60)):
            with self.subTest(rows=rows_count, ledger=ledger_count):
                partial = snapshot(rows_code=IOERR, ledger_code=IOERR)
                partial['rows'] = partial['rows'][:rows_count]
                partial['ledger'] = partial['ledger'][:ledger_count]
                self.assert_kind('read-error', partial, snapshot())

    def test_mixed_partial_rows_cannot_hide_behind_read_errors(self):
        partial = snapshot(20, rows_code=IOERR, ledger=[], ledger_code=IOERR)
        partial['rows'] = partial['rows'][:2]
        partial['rows'][1][4] = payload(19, 2).hex()
        self.assert_kind('wrong-data', partial, {'open': 14})

    def test_partial_rows_and_ledger_still_require_ordered_query_prefixes(self):
        rows_gap = snapshot(rows_code=IOERR, ledger=[], ledger_code=IOERR)
        rows_gap['rows'] = rows_gap['rows'][1:3]
        ledger_gap = snapshot(rows=[], rows_code=IOERR,
                              ledger=[[1, 1], [1, 3]], ledger_code=IOERR)
        for partial in (rows_gap, ledger_gap):
            with self.subTest(partial=partial):
                self.assert_kind('wrong-data', partial, {'open': 14})

    def test_partial_ledger_constrains_transaction_prefix(self):
        partial = snapshot(20, ledger=[[1, i] for i in range(1, 22)],
                           ledger_code=IOERR)
        self.assert_kind('wrong-data', partial, {'open': 14})

    def test_reopen_cannot_move_to_another_valid_prefix(self):
        self.assert_kind('wrong-data', snapshot(19), snapshot(20))
        first = snapshot(19, ledger=[], ledger_code=IOERR, rows_code=IOERR)
        first['rows'] = first['rows'][:1]
        second = snapshot(20, ledger=[], ledger_code=IOERR, rows_code=IOERR)
        second['rows'] = second['rows'][:1]
        self.assert_kind('wrong-data', first, second)

    def test_open_begin_and_end_errors_remain_distinct_from_verified_rows(self):
        self.assert_kind('open-refused', {'open': 14})
        self.assert_kind('read-error', {'open': 0, 'begin': 5}, snapshot())
        self.assert_kind('read-error', snapshot(), {'open': 14})
        self.assert_kind('read-error', snapshot(end=IOERR), snapshot())

    def test_missing_extra_or_malformed_protocol_fields_fail(self):
        cases = []
        for field in snapshot():
            item = snapshot()
            del item[field]
            cases.append((f'missing {field}', item))
        cases += [('extra', snapshot(unexpected=0)), ('bool', snapshot(zero=True)),
                  ('bool scan code', snapshot(rows_code=True)),
                  ('bool end code', snapshot(end=False)),
                  ('float', snapshot(cut=1.0)), ('negative', snapshot(cut=-1)),
                  ('rows object', snapshot(rows={})), ('ledger object', snapshot(ledger={})),
                  ('short row limit', snapshot(rows_code=ROW)),
                  ('short ledger limit', snapshot(ledger_code=ROW))]
        for name, item in cases:
            with self.subTest(name=name):
                self.assert_kind('invalid-result', item, snapshot())

    def test_malformed_row_and_ledger_transport_fail(self):
        cases = []
        for column, value in ((0, True), (1, True), (1, 2**63), (1, -(2**63) - 1),
                              (2, False), (3, True), (3, -1), (3, 1.0),
                              (4, 'gg'), (4, '0'), (4, None),
                              (4, '00' * (ROW_BYTES + 2))):
            item = snapshot()
            item['rows'][0][column] = value
            cases.append(item)
        for row in ([], [1, 1], [1, 1, 3, ROW_BYTES, payload(60, 1).hex(), 0]):
            item = snapshot()
            item['rows'][0] = row
            cases.append(item)
        for entry in ([1], [1, True], [1, 1, 0]):
            item = snapshot()
            item['ledger'][0] = entry
            cases.append(item)
        for index, item in enumerate(cases):
            with self.subTest(case=index):
                self.assert_kind('invalid-result', item, snapshot())

    def test_pass_count_and_failed_open_shape_are_strict(self):
        for passes in ((), (snapshot(),), (snapshot(), snapshot(), snapshot()),
                       ({'open': 14}, {'open': 14}), ({'open': 0},),
                       ({'open': True},), ({'open': 14, 'begin': 0},)):
            with self.subTest(count=len(passes)):
                self.assert_kind('invalid-result', *passes)

    def test_missing_truncated_duplicate_or_non_json_output_fails(self):
        complete = json.dumps({'passes': [snapshot(), snapshot()]})
        cases = ['', 'OK commit=60', complete[:-1], complete + complete,
                 'unexpected\n' + complete, '[]', '{}',
                 '{"passes":[{"open":NaN}]}', '{"passes":[{"open":Infinity}]}',
                 '{"passes":[{"open":14}],"passes":[{"open":14}]}',
                 '{"passes":[{"open":14,"open":14}]}']
        for output in cases:
            with self.subTest(length=len(output)):
                verdict = evaluate(0, output, '', history=self.history, native=True)
                self.assertEqual(verdict.kind, 'invalid-result', verdict.reason)
                self.assertTrue(verdict.failed)

    def test_result_without_external_history_cannot_be_verified(self):
        verdict = evaluate(0, json.dumps({'passes': [snapshot(), snapshot()]}),
                           '', history=None, native=True)
        self.assertEqual(verdict.kind, 'invalid-result')
        self.assertTrue(verdict.failed)

    def test_process_failures_override_even_valid_result_evidence(self):
        for code, diagnostic, kind in ((None, '', 'timeout'),
                                       (1, '', 'process-error'),
                                       (-9, '', 'process-error'),
                                       (0, 'thread panicked', 'process-error')):
            for native in (False, True):
                with self.subTest(code=code, native=native):
                    verdict = self.verdict(snapshot(), snapshot(), code=code,
                                           diagnostic=diagnostic, native=native)
                    self.assertEqual(verdict.kind, kind)
                    self.assertTrue(verdict.failed)


class SoakHistoryTests(unittest.TestCase):
    def test_history_replays_recorded_intents_and_preserves_old_snapshots(self):
        history = load_history(HISTORY_TEXT)
        self.assertEqual(history.acknowledged, COMMITS)
        self.assertEqual(history.rows, ROWS)
        self.assertEqual(history.row_bytes, ROW_BYTES)
        self.assertEqual(len(history.snapshots), COMMITS + 1)
        self.assertEqual(history.snapshots[0][0], payload(0, 1))
        self.assertEqual(history.snapshots[20][0], payload(20, 1))
        self.assertEqual(history.snapshots[60][3], payload(0, 4))
        with self.assertRaises(TypeError):
            history.snapshots[0][0] = b'changed'

    def test_missing_acknowledgements_and_incomplete_history_are_rejected(self):
        for commit in (0, 20, 60):
            for ack in (None, False, 1, 'true'):
                with self.subTest(commit=commit, ack=ack):
                    records = copy.deepcopy(RECORDS)
                    if ack is None:
                        del records[commit + 1]['ack']
                    else:
                        records[commit + 1]['ack'] = ack
                    with self.assertRaises(ValueError):
                        load_history(history_text(records))
        for records in (RECORDS[:-1], RECORDS[:1], RECORDS + [RECORDS[-1]]):
            with self.subTest(length=len(records)):
                with self.assertRaises(ValueError):
                    load_history(history_text(records))

    def test_invalid_header_and_commit_sequence_are_rejected(self):
        for field, value in (('format', 2), ('rows', 299), ('commits', 59),
                             ('row_bytes', 1501), ('rows', True), ('extra', 0)):
            with self.subTest(field=field, value=value):
                records = copy.deepcopy(RECORDS)
                records[0][field] = value
                with self.assertRaises(ValueError):
                    load_history(history_text(records))
        for commit in (True, -1, 0, 2):
            with self.subTest(commit=commit):
                records = copy.deepcopy(RECORDS)
                records[2]['commit'] = commit
                with self.assertRaises(ValueError):
                    load_history(history_text(records))

    def test_incomplete_baseline_and_invalid_update_intents_are_rejected(self):
        cases = []
        for index in (1, 2):
            missing = copy.deepcopy(RECORDS)
            missing[index]['rows'].pop()
            cases.append(missing)
            duplicate = copy.deepcopy(RECORDS)
            duplicate[index]['rows'][1] = duplicate[index]['rows'][0][:]
            cases.append(duplicate)
            for row_id in (True, 0, ROWS + 1):
                unknown = copy.deepcopy(RECORDS)
                unknown[index]['rows'][0][0] = row_id
                cases.append(unknown)
        unordered = copy.deepcopy(RECORDS)
        unordered[1]['rows'].reverse()
        cases.append(unordered)
        for index, records in enumerate(cases):
            with self.subTest(case=index):
                with self.assertRaises(ValueError):
                    load_history(history_text(records))

    def test_history_payload_and_json_syntax_are_strict(self):
        for value in ('gg' * ROW_BYTES, '00' * (ROW_BYTES - 1),
                      '00' * (ROW_BYTES + 1), '0', None):
            with self.subTest(value_type=type(value).__name__):
                records = copy.deepcopy(RECORDS)
                records[2]['rows'][0][1] = value
                with self.assertRaises(ValueError):
                    load_history(history_text(records))
        cases = ['', HISTORY_TEXT[:-3], HISTORY_TEXT + 'garbage\n',
                 HISTORY_TEXT.replace('"format":1', '"format":1,"format":1', 1),
                 HISTORY_TEXT.replace('"ack":true', '"ack":true,"ack":true', 1)]
        for text in cases:
            with self.subTest(length=len(text)):
                with self.assertRaises(ValueError):
                    load_history(text)


if __name__ == '__main__':
    unittest.main()
