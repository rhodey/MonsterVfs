"""Crash-image checks require both successful output and successful process exit."""
import ast
import collections
import concurrent.futures
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import types
import unittest


class CrashVerdictTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = pathlib.Path(__file__).resolve().parents[2] / 'tools/test-crash'
        tree = ast.parse(path.read_text())
        # Load the real checking path without running the tool's builds and campaign.
        functions = ast.Module(body=[node for node in tree.body
            if isinstance(node, ast.FunctionDef)
            and node.name in {'materialize', 'verify', 'collect'}], type_ignores=[])
        cls.checking = compile(functions, str(path), 'exec')

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.host = self.root / 'host'
        self.host.mkdir()
        self.work = self.root / 'images'
        self.work.mkdir()
        child = self.host / 'crash-check'
        child.write_text("""import os, signal, sys, time
mode, reopen = sys.argv[1], sys.argv[-1]
if mode == 'empty': sys.exit(0)
if mode == 'fail': print('FAIL damaged image'); sys.exit(0)
print('OK highest=1', flush=True)
print('child diagnostic', file=sys.stderr, flush=True)
if mode == 'timeout': time.sleep(30)
if mode == 'signal': os.kill(os.getpid(), signal.SIGTERM)
if mode == 'exit7' or mode == 'stop-' + reopen: sys.exit(7)
""")
        self.calls = []
        self.completed = []
        def run(command, **kwargs):
            self.calls.append(command[-1])
            if command[1] == 'timeout':
                kwargs['timeout'] = 0.25
            result = subprocess.run([sys.executable, *command], **kwargs)
            self.completed.append(result)
            return result
        self.namespace = {
            'pathlib': pathlib, 'shutil': shutil,
            'subprocess': types.SimpleNamespace(run=run,
                TimeoutExpired=subprocess.TimeoutExpired),
            'mvbuild': types.SimpleNamespace(host=self.host),
            'args': types.SimpleNamespace(every=16, busy_ms=50),
        }
        exec(self.checking, self.namespace)

    def check(self, mode, number=1):
        self.calls.clear()
        self.completed.clear()
        verdict = self.namespace['verify'](self.work, mode, 'monstervfs_parity', number,
            (0, 'clean', 1, {'/database': b'image'}))
        future = concurrent.futures.Future()
        future.set_result(verdict)
        totals, failures, examples = collections.Counter(), collections.Counter(), {}
        self.namespace['collect']([future], totals, failures, examples,
            [['W', '/database', '0', '5']])
        self.assertFalse((self.work / str(number)).exists())
        return verdict[2], totals

    def test_success_needs_all_requested_reopens(self):
        for number, calls in [(1, ['shared']), (0, ['readonly', 'exclusive', 'shared'])]:
            with self.subTest(number=number):
                line, totals = self.check('ok', number)
                self.assertEqual(totals, {'ok': 1})
                self.assertEqual(line, 'OK highest=1')
                self.assertEqual(self.calls, calls)

    def test_nonzero_exit_and_signal_override_ok_output(self):
        for mode, code in [('exit7', 7), ('signal', -signal.SIGTERM)]:
            with self.subTest(mode=mode):
                line, totals = self.check(mode)
                self.assertEqual(self.completed[-1].returncode, code)
                self.assertEqual(totals, {'failed': 1})
                self.assertIn(f'exit={code}', line)
                self.assertIn('child diagnostic', line)

    def test_successful_exit_with_empty_or_failed_output_is_rejected(self):
        for mode in ('empty', 'fail'):
            with self.subTest(mode=mode):
                line, totals = self.check(mode)
                self.assertEqual(self.completed[-1].returncode, 0)
                self.assertEqual(totals, {'failed': 1})
                if mode == 'fail':
                    self.assertIn('FAIL damaged image', line)

    def test_timeout_does_not_accept_prior_ok_output(self):
        line, totals = self.check('timeout')
        self.assertEqual(totals, {'failed': 1})
        self.assertEqual(line, 'FAIL timeout')

    def test_failed_reopen_stops_before_other_modes(self):
        for mode, calls in [('stop-readonly', ['readonly']),
                            ('stop-exclusive', ['readonly', 'exclusive'])]:
            with self.subTest(mode=mode):
                line, totals = self.check(mode, 0)
                self.assertEqual(totals, {'failed': 1})
                self.assertIn('exit=7', line)
                self.assertEqual(self.calls, calls)


if __name__ == '__main__':
    unittest.main()
