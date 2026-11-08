import json
import os
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
import mvtcl


class TclToolchainTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()

    def shell(self, name, version, tailcall=1):
        executable = self.root / name
        executable.write_text(f'#!{sys.executable}\n' +
            'import json, os, sys\n'
            'if len(sys.argv) == 1:\n'
            '    sys.stdin.read()\n' +
            f'    print({version + " " + str(tailcall)!r})\n' +
            'else:\n'
            '    with open(os.environ["MVTCL_TRACE"], "w") as stream:\n'
            '        json.dump({"argv": sys.argv, "wrapper": os.environ.get("WRAPPER"), "cwd": os.getcwd()}, stream)\n')
        executable.chmod(0o755)
        return str(executable)

    def test_explicit_old_shell_rejected_without_fallback(self):
        old = self.shell('old tcl', '8.5.9', tailcall=0)
        with self.assertRaisesRegex(SystemExit, r'Tcl 8.6.*unsupported Tcl.*8.5.9'):
            mvtcl.select_tclsh(old)

    def test_automatic_selection_skips_old_shell(self):
        old = self.shell('old', '8.5.9', tailcall=0)
        supported = self.shell('supported', '8.6.18')
        paths = {'tclsh8.6': old, 'tclsh': supported}
        with patch('mvtcl.shutil.which', side_effect=paths.get):
            self.assertEqual(mvtcl.select_tclsh(), supported)

    def test_missing_explicit_shell_has_actionable_error(self):
        with self.assertRaisesRegex(SystemExit, '--with-tclsh PATH.*executable not found'):
            mvtcl.select_tclsh(str(self.root / 'missing'))

    def test_same_explicit_shell_drives_bootstrap_and_codegen(self):
        shell = self.shell('selected tcl', '8.6.18')
        selected = mvtcl.select_tclsh(shell)
        self.assertEqual(selected, shell)
        source = self.root / 'sqlite source'
        build = self.root / 'build directory'
        build.mkdir()
        development = self.root / 'tcl development'
        trace = self.root / 'invocation.json'
        with patch.dict(os.environ, {'MVTCL_TRACE': str(trace)}):
            mvtcl.configure(source, build, selected, str(development))
        invocation = json.loads(trace.read_text())
        self.assertEqual(invocation, {
            'argv': [shell, str(source / 'autosetup/autosetup'), '--enable-threadsafe',
                     '--disable-shared', '--with-tclsh=' + shell,
                     '--with-tcl=' + str(development)],
            'wrapper': str(source / 'configure'),
            'cwd': str(build),
        })


if __name__ == '__main__':
    unittest.main()
