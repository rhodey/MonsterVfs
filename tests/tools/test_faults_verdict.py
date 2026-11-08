import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from faults_verdict import matches


class FaultVerdictTests(unittest.TestCase):
    def test_expected_fields(self):
        self.assertTrue(matches('count=50 bytes=150000 integrity=ok close=0',
                                'count=50 bytes=150000 integrity=ok'))

    def test_one_success_does_not_cover_other_open(self):
        outcomes = ['count=50 bytes=150000 integrity=ok close=0', 'open=E8458 close=0']
        self.assertEqual([matches(result, 'count=50 bytes=150000 integrity=ok')
                          for result in outcomes], [True, False])

    def test_error_codes_match_whole_fields(self):
        self.assertTrue(matches('count=E11 close=0', 'count=E11'))
        self.assertFalse(matches('count=E118 close=0', 'count=E11'))

    def test_failed_child_is_not_success(self):
        for result in ('', 'TIMEOUT', 'count=50 integrity=ok [exit -11]',
                       'count=50 integrity=ok [exit 1 failed]'):
            self.assertFalse(matches(result, 'integrity=ok'))

    def test_missing_or_different_field(self):
        self.assertFalse(matches('count=50 close=0', 'integrity=ok'))
        self.assertFalse(matches('count=49 bytes=150000 integrity=ok', 'count=50'))


if __name__ == '__main__':
    unittest.main()
