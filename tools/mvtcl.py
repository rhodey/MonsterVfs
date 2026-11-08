"""Choose one canonical Tcl for SQLite configure and testfixture generation."""
import os
import pathlib
import re
import shutil
import subprocess


def select_tclsh(requested=None):
    candidates = [requested] if requested else ['tclsh9.1', 'tclsh9.0', 'tclsh8.6', 'tclsh']
    failures = []
    for candidate in candidates:
        executable = shutil.which(os.path.expanduser(candidate))
        if not executable:
            if requested:
                failures.append(f'{candidate}: executable not found')
            continue
        executable = str(pathlib.Path(executable).resolve())
        try:
            result = subprocess.run([executable], input='puts "[info patchlevel] [llength [info commands tailcall]]"\n',
                                    text=True, capture_output=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired) as error:
            failures.append(f'{executable}: {error}')
            continue
        version = re.fullmatch(r'(\d+)\.(\d+)\S* 1', result.stdout.strip())
        if result.returncode == 0 and version and tuple(map(int, version.groups())) >= (8, 6):
            return executable
        detail = result.stderr.strip() or result.stdout.strip() or f'exit {result.returncode}'
        failures.append(f'{executable}: unsupported Tcl ({detail})')
    detail = '; '.join(failures) or 'no Tcl shell found on PATH'
    raise SystemExit(f'Tcl 8.6 or newer is required; select it with --with-tclsh PATH. {detail}')


def configure(source, build, tclsh, with_tcl=None):
    # --with-tclsh alone does not select the interpreter running configure.
    # WRAPPER retains autosetup's normal source-directory and configure identity.
    command = [tclsh, str(source/'autosetup/autosetup'), '--enable-threadsafe',
               '--disable-shared', '--with-tclsh='+tclsh]
    if with_tcl:
        command.append('--with-tcl='+str(pathlib.Path(with_tcl).expanduser().resolve()))
    subprocess.run(command, cwd=build, check=True,
                   env={**os.environ, 'WRAPPER': str(source/'configure')})
