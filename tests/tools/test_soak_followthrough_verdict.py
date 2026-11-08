"""Follow-up writes restrict which complete transaction states may reappear."""
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from soak_verdict import History, evaluate


DONE, IOERR = 101, 266
HISTORY = History((
    (b'base', b'base', b'base'),
    (b'old1', b'old2', b'old3'),
    (b'new1', b'old2', b'new3'),
), row_bytes=4)


def snapshot(commit, **changes):
    result = {
        'open': 0, 'begin': 0,
        'rows': [[1, index, 3, len(data), data.hex()]
                 for index, data in enumerate(HISTORY.snapshots[commit], 1)],
        'rows_code': DONE,
        'ledger': [[1, value] for value in range(1, commit + 1)],
        'ledger_code': DONE, 'end': 0, 'zero': 0, 'cut': 0,
    }
    result.update(changes)
    return result


class FollowthroughVerdictTests(unittest.TestCase):
    def verdict(self, *passes, **options):
        return evaluate(0, json.dumps({'passes': passes}), '',
                        history=HISTORY, **options)

    def assert_kind(self, kind, *passes, **options):
        verdict = self.verdict(*passes, **options)
        self.assertEqual(verdict.kind, kind, verdict.reason)
        self.assertEqual(verdict.failed,
                         kind == 'invalid-result' or
                         (kind == 'wrong-data' and not options.get('native', False)),
                         verdict.reason)
        return verdict

    def test_acknowledged_followup_requires_exact_new_state(self):
        verdict = self.assert_kind('exact-recovery', snapshot(2), snapshot(2),
                                   allowed_commits={2})
        self.assertEqual(verdict.fields['commit'], 2)
        self.assertEqual(verdict.fields['lost_commits'], 0)
        for commit in (0, 1):
            with self.subTest(commit=commit):
                self.assert_kind('wrong-data', snapshot(commit), snapshot(commit),
                                 allowed_commits={2})

    def test_failed_precommit_write_requires_unchanged_state(self):
        self.assert_kind('prefix-loss', snapshot(1), snapshot(1), allowed_commits={1})
        self.assert_kind('wrong-data', snapshot(2), snapshot(2), allowed_commits={1})

    def test_ambiguous_commit_accepts_old_or_entire_new_transaction(self):
        for commit, kind in ((1, 'prefix-loss'), (2, 'exact-recovery')):
            with self.subTest(commit=commit):
                verdict = self.assert_kind(kind, snapshot(commit), snapshot(commit),
                                           allowed_commits={1, 2})
                self.assertEqual(verdict.fields['commit'], commit)
        self.assert_kind('wrong-data', snapshot(0), snapshot(0), allowed_commits={1, 2})

    def test_ambiguous_commit_cannot_move_between_allowed_reopens(self):
        self.assert_kind('wrong-data', snapshot(1), snapshot(2), allowed_commits={1, 2})

    def test_mixed_followup_is_not_an_allowed_old_or_new_transaction(self):
        mixed = snapshot(2)
        mixed['rows'][0][4] = HISTORY.snapshots[1][0].hex()
        self.assert_kind('wrong-data', mixed, mixed, allowed_commits={1, 2})

    def test_partial_contradictions_are_not_excused_by_read_failure(self):
        old = snapshot(1, rows_code=IOERR, ledger=[], ledger_code=IOERR)
        old['rows'] = old['rows'][:1]
        self.assert_kind('wrong-data', old, {'open': 14}, allowed_commits={2})
        mixed = snapshot(2, rows_code=IOERR, ledger=[], ledger_code=IOERR)
        mixed['rows'][0][4] = HISTORY.snapshots[1][0].hex()
        self.assert_kind('wrong-data', mixed, {'open': 14}, allowed_commits={1, 2})

    def test_partial_compatible_read_does_not_claim_followup_verified(self):
        partial = snapshot(2, rows_code=IOERR, ledger=[], ledger_code=IOERR)
        partial['rows'] = partial['rows'][:1]
        verdict = self.assert_kind('read-error', partial, snapshot(2), allowed_commits={2})
        self.assertIsNone(verdict.fields['commit'])
        self.assertIsNone(verdict.fields['lost_commits'])
        self.assert_kind('open-refused', {'open': 14}, allowed_commits={2})

    def test_rows_and_ledger_must_describe_the_same_allowed_commit(self):
        wrong = snapshot(2, ledger=snapshot(1)['ledger'])
        self.assert_kind('wrong-data', wrong, wrong, allowed_commits={1, 2})
        wrong = snapshot(1, ledger=snapshot(2)['ledger'])
        self.assert_kind('wrong-data', wrong, wrong, allowed_commits={1, 2})

    def test_allowed_set_is_validated_even_when_open_fails(self):
        invalid = [set(), frozenset(), [], [1], (1,), {1: True}, 1, '1',
                   {-1}, {3}, {True}, {False}, {1.0}, {None}, {1, '2'}]
        for allowed in invalid:
            with self.subTest(allowed=repr(allowed)):
                self.assert_kind('invalid-result', snapshot(2), snapshot(2),
                                 allowed_commits=allowed)
                self.assert_kind('invalid-result', {'open': 14}, allowed_commits=allowed)

    def test_allowed_set_is_not_mutated_and_frozenset_is_supported(self):
        allowed = {1, 2}
        self.assert_kind('exact-recovery', snapshot(2), snapshot(2), allowed_commits=allowed)
        self.assertEqual(allowed, {1, 2})
        self.assert_kind('prefix-loss', snapshot(1), snapshot(1),
                         allowed_commits=frozenset({1}))

    def test_default_still_accepts_every_recorded_prefix(self):
        for commit in range(HISTORY.acknowledged + 1):
            with self.subTest(commit=commit):
                kind = 'exact-recovery' if commit == HISTORY.acknowledged else 'prefix-loss'
                self.assert_kind(kind, snapshot(commit), snapshot(commit))
                self.assert_kind(kind, snapshot(commit), snapshot(commit), allowed_commits=None)

    def test_native_reports_forbidden_state_without_excusing_bad_protocol(self):
        self.assert_kind('wrong-data', snapshot(1), snapshot(1),
                         allowed_commits={2}, native=True)
        self.assert_kind('invalid-result', snapshot(2), snapshot(2),
                         allowed_commits=set(), native=True)


if __name__ == '__main__':
    unittest.main()
