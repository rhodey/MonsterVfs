"""Deliberately broken stress results must not become passing comparisons."""
import pathlib
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from stress_verdict import assess, comparison, run

THREAD_OK = 'Running walthread5 for 1 seconds...\n0 errors out of 1 tests\n'
MULTI_OK = 'Summary: 0 errors out of 30 tests\nEND: mptest mp.db script.test\n'


class StressVerdictTests(unittest.TestCase):
    def test_clean_programs_need_real_completion(self):
        self.assertTrue(assess('threadtest3', 0, THREAD_OK).passed)
        self.assertEqual(assess('mptest', 0, MULTI_OK).tests, 30)
        self.assertTrue(assess('mptest', 0, MULTI_OK).passed)

    def test_reported_errors_override_successful_exit(self):
        for kind, output in [('threadtest3', THREAD_OK), ('mptest', MULTI_OK)]:
            with self.subTest(kind=kind):
                self.assertFalse(assess(kind, 0, output.replace('0 errors', '1 errors')).passed)

    def test_failure_lines_override_clean_summary(self):
        for diagnostic in ['FAILED: assertion', 'Failure: wrong data', 'Error: (1) bad',
                           '00001.client01:ERROR: mismatch', '00001.mptest:FATAL: failed',
                           'Assertion result == expected failed']:
            with self.subTest(diagnostic=diagnostic):
                self.assertFalse(assess('threadtest3', 0, diagnostic+'\n'+THREAD_OK).passed)

    def test_empty_missing_zero_or_duplicate_summary_is_not_pass(self):
        for output in ['', 'Running walthread5 for 1 seconds...\n',
                       '0 errors out of 0 tests\n', THREAD_OK+THREAD_OK,
                       '0 errors out of not-a-number tests\n']:
            with self.subTest(output=output):
                self.assertFalse(assess('threadtest3', 0, output).passed)

    def test_test_that_started_after_summary_did_not_complete(self):
        self.assertFalse(assess('threadtest3', 0,
                               '0 errors out of 1 tests\nRunning walthread5 for 1 seconds...\n').passed)
        self.assertFalse(assess('threadtest3', 0, THREAD_OK.replace('1 tests','2 tests')).passed)

    def test_mptest_requires_its_final_end_record(self):
        self.assertFalse(assess('mptest', 0, MULTI_OK.split('END:')[0]).passed)
        self.assertFalse(assess('mptest', 0,
                               'END: mptest\nSummary: 0 errors out of 30 tests\n').passed)

    def test_timeout_signal_or_nonzero_exit_does_not_pass(self):
        for code in ['timeout', -9, 1, 255]:
            with self.subTest(code=code):
                self.assertFalse(assess('threadtest3', code, THREAD_OK).passed)

    def test_native_failures_never_become_success(self):
        good = assess('threadtest3', 0, THREAD_OK)
        bad = assess('threadtest3', 255, THREAD_OK)
        self.assertEqual(comparison(good, good), 'passed')
        self.assertEqual(comparison(good, bad), 'failed')
        self.assertEqual(comparison(bad, good), 'inconclusive')
        self.assertEqual(comparison(bad, bad), 'inconclusive')

    def test_incomplete_assertion_count_cannot_pass_native_comparison(self):
        native = assess('mptest', 0, MULTI_OK)
        partial = assess('mptest', 0, MULTI_OK.replace('30 tests', '1 tests'))
        self.assertTrue(partial.passed)
        self.assertEqual(comparison(native, partial), 'failed')

    def test_successful_process_with_failure_summary_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            code, output = run([sys.executable, '-c',
                                "print('Summary: 1 errors out of 2 tests\\nEND: mptest')"],
                               directory, {}, 5)
        self.assertEqual(code, 0)
        self.assertFalse(assess('mptest', code, output).passed)

    def test_native_run_can_remove_inherited_extension_selection(self):
        import os
        from unittest.mock import patch
        with patch.dict(os.environ, {'MONSTERVFS_EXTENSION': 'wrong-library'}):
            with tempfile.TemporaryDirectory() as directory:
                code, output = run([sys.executable, '-c',
                                    "import os; print('MONSTERVFS_EXTENSION' in os.environ)"],
                                   directory, {'MONSTERVFS_EXTENSION': None}, 5)
        self.assertEqual((code, output.strip()), (0, 'False'))

    def test_interruption_terminates_owned_group_and_preserves_original_error(self):
        import os
        import signal
        from unittest.mock import patch
        script = """import subprocess, sys, time
subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
print('clients ready', flush=True)
time.sleep(30)
"""
        for failure in [KeyboardInterrupt(), RuntimeError('injected pipe failure')]:
            with self.subTest(failure=type(failure).__name__):
                processes = []
                def interrupt(process, *args, **kwargs):
                    processes.append(process)
                    self.assertEqual(process.stdout.readline().strip(), 'clients ready')
                    raise failure
                try:
                    with tempfile.TemporaryDirectory() as directory:
                        with patch('stress_verdict.subprocess.Popen.communicate', interrupt):
                            with patch('stress_verdict.os.killpg', wraps=os.killpg) as kill:
                                with self.assertRaises(type(failure)) as raised:
                                    run([sys.executable, '-c', script], directory, {}, 5)
                                self.assertIs(raised.exception, failure)
                                process = processes[0]
                                kill.assert_called_once_with(process.pid, signal.SIGKILL)
                                self.assertEqual(process.returncode, -signal.SIGKILL)
                                self.assertTrue(process.stdout.closed)
                finally:
                    for process in processes:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        process.wait()
                        process.stdout.close()

    def test_cleanup_errors_do_not_replace_original_interruption(self):
        from unittest.mock import Mock, patch
        failure = KeyboardInterrupt()
        process = Mock(pid=123)
        process.communicate.side_effect = failure
        process.wait.side_effect = OSError('reap failed')
        process.stdout.close.side_effect = OSError('pipe close failed')
        with patch('stress_verdict.subprocess.Popen', return_value=process):
            with patch('stress_verdict.os.killpg', side_effect=OSError('kill failed')):
                with self.assertRaises(KeyboardInterrupt) as raised:
                    run(['unused'], '.', {}, 5)
        self.assertIs(raised.exception, failure)
        process.wait.assert_called_once_with()
        process.stdout.close.assert_called_once_with()

    def test_timeout_retains_output_and_terminates_client_pipe_holders(self):
        script = """import subprocess, sys, time
subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
print('workload started', flush=True)
time.sleep(30)
"""
        started = time.monotonic()
        with tempfile.TemporaryDirectory() as directory:
            code, output = run([sys.executable, '-c', script], directory, {}, 1)
        self.assertEqual(code, 'timeout')
        self.assertIn('workload started', output)
        self.assertLess(time.monotonic()-started, 10)


if __name__ == '__main__':
    unittest.main()
