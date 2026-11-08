"""Which build a tool works on. MONSTERVFS_BUILD picks parity, the default, or copy."""
import os, pathlib, shutil, subprocess, sys

root = pathlib.Path(__file__).resolve().parent.parent
NAME = os.environ.get('MONSTERVFS_BUILD', 'parity')
if NAME not in ('parity', 'copy'):
    raise SystemExit(f'MONSTERVFS_BUILD must be parity or copy, not {NAME}')
VFS = f'monstervfs_{NAME}'
ENTRY = 'sqlite3_extension_init'
SUFFIX = 'dylib' if sys.platform == 'darwin' else 'so'
# Test hosts differ per build, so that both can be built at once.
host = root/'target/sqlite'/NAME
host.mkdir(parents=True, exist_ok=True)
CFLAGS = [f'-DMVFS_NAME="{VFS}"', f'-DMVFS_ENTRY="{ENTRY}"', f'-DMVFS_COPY={int(NAME == "copy")}']

def cargo(linkage='loadable', *extra, build=NAME):
    """Arguments that select a build, with its own target directory per linkage."""
    features = ','.join([linkage, build, *extra])
    suffix = ''.join(f'-{name}' for name in extra)
    return ['--no-default-features', '--features', features,
            '--target-dir', str(root/'target'/build/f'{linkage}{suffix}')]

def released():
    """Where library(release=True) leaves its file."""
    return root/'target'/NAME/'loadable/release'/f'libmonstervfs_{NAME}.{SUFFIX}'

def library(given=None, release=False, extra=(), build=NAME):
    """The path of a loadable library, built here unless one is given. Cargo names its
    output after the crate, so this copy is what carries the build's name."""
    given = given or os.environ.get('MONSTERVFS_TEST_LIBRARY')
    if given:
        return str(pathlib.Path(given).resolve())
    arguments = cargo('loadable', *extra, build=build)
    subprocess.run(['cargo', 'build', '--lib', *(['--release'] if release else []), *arguments], cwd=root, check=True)
    out = pathlib.Path(arguments[-1])/('release' if release else 'debug')
    named = out/f'libmonstervfs_{build}.{SUFFIX}'
    shutil.copy2(out/f'libmonstervfs.{SUFFIX}', named)
    return str(named)
