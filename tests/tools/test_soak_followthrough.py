"""Follow-through failures must never excuse losing an acknowledged transaction."""
import json
from pathlib import Path
import tempfile
import unittest

from test_soak_verdict import HISTORY_TEXT
from soak_verdict import load_history
from soak_followthrough import operation, plan, run


class FollowthroughTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.history = load_history(HISTORY_TEXT)

    def observation(self, history, commit, partial=False):
        rows = [[1, i + 1, 3, history.row_bytes, value.hex()]
                for i, value in enumerate(history.snapshots[commit])]
        snapshot = dict(open=0, begin=0, rows=rows,
                        rows_code=266 if partial else 101,
                        ledger=[[1, i] for i in range(1, commit + 1)],
                        ledger_code=101, end=0, zero=0, cut=0)
        return 0, json.dumps({'passes': [snapshot, snapshot]}), ''

    def execute(self, base=60, write=None, checkpoint=None, reads=None, native=False):
        intent, history = plan(self.history, base)
        reads = list(reads or [self.observation(history, base + 1)] * 2)
        write = write if write is not None else {'stage': 'commit', 'code': 0}
        checkpoint = checkpoint if checkpoint is not None else {
            'stage': 'checkpoint', 'code': 0, 'log': 0, 'checkpointed': 0}
        calls = []
        with tempfile.TemporaryDirectory() as directory:
            db = Path(directory) / 'db'

            def invoke(mode, *args):
                calls.append(mode)
                if mode == 'write':
                    self.assertEqual(int(args[1]), base + 1)
                    self.assertEqual(Path(args[2]).read_bytes(),
                                     b''.join(bytes.fromhex(row[1]) for row in intent['rows']))
                    self.assertEqual(json.loads((db.parent / 'followup.json').read_text()), intent)
                    return 0, json.dumps(write), ''
                if mode == 'checkpoint':
                    return 0, json.dumps(checkpoint), ''
                self.assertEqual(mode, 'inspect')
                self.assertEqual(int(args[1]), base + 1)
                return reads.pop(0)

            verdict, evidence = run(invoke, db, self.history, base, native=native)
        return verdict, evidence, calls

    def test_clean_latest_and_old_prefix_are_usable(self):
        for base in (0, 30, 60):
            verdict, evidence, calls = self.execute(base=base)
            self.assertEqual(verdict.kind, 'usable', verdict)
            self.assertFalse(verdict.failed)
            self.assertTrue(verdict.fields['write_acknowledged'])
            self.assertEqual(calls, ['write', 'inspect', 'checkpoint', 'inspect'])
            self.assertEqual(evidence['intent']['base_commit'], base)

    def test_acknowledged_write_cannot_disappear_before_checkpoint(self):
        _, history = plan(self.history, 60)
        verdict, _, calls = self.execute(reads=[self.observation(history, 60)])
        self.assertEqual(verdict.kind, 'wrong-data')
        self.assertTrue(verdict.failed)
        self.assertNotIn('checkpoint', calls)

    def test_acknowledged_write_survives_successful_and_failed_checkpoint(self):
        _, history = plan(self.history, 60)
        for code in (0, 5, 266):
            checkpoint = {'stage': 'checkpoint', 'code': code, 'log': 0, 'checkpointed': 0}
            verdict, _, _ = self.execute(checkpoint=checkpoint, reads=[
                self.observation(history, 61), self.observation(history, 60)])
            self.assertEqual(verdict.kind, 'wrong-data')
            self.assertTrue(verdict.failed)

    def test_failed_commit_allows_old_or_entire_new_transaction(self):
        _, history = plan(self.history, 60)
        for commit in (60, 61):
            verdict, _, calls = self.execute(write={'stage': 'commit', 'code': 266},
                                            reads=[self.observation(history, commit)])
            self.assertEqual(verdict.kind, 'write-error')
            self.assertFalse(verdict.failed)
            self.assertFalse(verdict.fields['write_acknowledged'])
            self.assertEqual(calls, ['write', 'inspect'])

    def test_precommit_failure_requires_unchanged_state(self):
        _, history = plan(self.history, 60)
        for commit, expected in ((60, 'write-error'), (61, 'wrong-data')):
            verdict, _, _ = self.execute(write={'stage': 'update', 'code': 19},
                                        reads=[self.observation(history, commit)])
            self.assertEqual(verdict.kind, expected)
            self.assertEqual(verdict.failed, expected == 'wrong-data')

    def test_partial_reads_are_not_usable_or_checkpoint_authority(self):
        _, history = plan(self.history, 60)
        partial = self.observation(history, 61, partial=True)
        verdict, _, calls = self.execute(reads=[partial])
        self.assertEqual(verdict.kind, 'reopen-error')
        self.assertFalse(verdict.failed)
        self.assertNotIn('checkpoint', calls)

    def test_wrong_readable_data_wins_over_write_error(self):
        _, history = plan(self.history, 60)
        _, text, _ = self.observation(history, 61)
        data = json.loads(text)
        data['passes'][0]['rows'][149][4] = history.snapshots[60][149].hex()
        verdict, _, _ = self.execute(write={'stage': 'commit', 'code': 266},
                                    reads=[(0, json.dumps(data), '')])
        self.assertEqual(verdict.kind, 'wrong-data')
        self.assertTrue(verdict.failed)

    def test_native_wrong_data_is_reported_but_not_failed(self):
        _, history = plan(self.history, 60)
        verdict, _, _ = self.execute(reads=[self.observation(history, 60)], native=True)
        self.assertEqual(verdict.kind, 'wrong-data')
        self.assertFalse(verdict.failed)

    def test_checkpoint_errors_and_no_wal_result_are_not_usable(self):
        for checkpoint in ({'stage': 'checkpoint', 'code': 5, 'log': 9, 'checkpointed': 4},
                           {'stage': 'checkpoint', 'code': 0, 'log': -1, 'checkpointed': -1},
                           {'stage': 'open', 'code': 14}):
            verdict, _, calls = self.execute(checkpoint=checkpoint)
            self.assertEqual(verdict.kind, 'checkpoint-error')
            self.assertFalse(verdict.failed)
            self.assertEqual(calls[-1], 'inspect')

    def test_missing_or_invalid_operation_transcripts_fail(self):
        cases = [({}, False), ({'stage': 'commit', 'code': False}, False),
                 ({'stage': 'update', 'code': 0}, False),
                 ({'stage': 'commit', 'code': 101}, False),
                 ({'stage': 'checkpoint', 'code': 0}, True),
                 ({'stage': 'checkpoint', 'code': 0, 'log': -2, 'checkpointed': 0}, True)]
        for value, is_checkpoint in cases:
            result, failure = operation((0, json.dumps(value), ''), checkpoint=is_checkpoint)
            self.assertIsNone(result)
            self.assertEqual(failure.kind, 'invalid-result')
            self.assertTrue(failure.failed)
        for outcome in ((None, '', ''), (1, '{}', ''), (0, '{}', 'thread panicked')):
            result, failure = operation(outcome)
            self.assertIsNone(result)
            self.assertTrue(failure.failed)


if __name__ == '__main__':
    unittest.main()
