# Build, use, and test

Run these commands from the repository root. Use the pinned Rust 1.94.0 toolchain,
a C compiler, and Python 3 on Linux or macOS. Building test hosts also downloads
and verifies the pinned SQLite sources. Normal builds use checked-in bindings;
only `tools/generate-bindings` needs libclang.

## Building

```sh
tools/build-library
```

This builds both release libraries. Set `MONSTERVFS_BUILD=parity` or
`MONSTERVFS_BUILD=copy` to select one. Other single-format tools default to
`parity`; plain Cargo also defaults to `loadable,parity`.

Libraries are named `libmonstervfs_parity.so` and `libmonstervfs_copy.so`
(`.dylib` on macOS), under `target/<format>/loadable/release/`. For static linking:

```sh
cargo build --release --no-default-features --features static,copy --target-dir target/copy/static
```

The static library is `target/copy/static/release/libmonstervfs.a`. Substitute
`parity` for the other format; a static host links one format at a time.

The independent test host uses SQLite 3.54.0 with DBPAGE, threaded connections,
and extension loading. It is separate from the loadable library, which uses its
host's SQLite API. Build and check both linkage modes for one format with:

```sh
MONSTERVFS_BUILD=copy tools/build-host --static
MONSTERVFS_BUILD=copy tools/test-build
```

`tools/smoke` runs the already-built ABI hosts. `tools/check-abi LIBRARY` checks
exports and rejects a loadable library linked to a second SQLite engine.
On macOS, native code uses Rust's deployment target; set
`MACOSX_DEPLOYMENT_TARGET` to override it consistently.

## Loading and initialization

The host must match the pinned SQLite version/source ID and enable
`SQLITE_ENABLE_DBPAGE_VTAB`; see [ARCH.md](../ARCH.md#host-and-registration).
Load either library using SQLite's standard entry point: pass NULL for the
entry-point argument to `sqlite3_load_extension`. Loading registers
`monstervfs_parity` or `monstervfs_copy` without changing the default VFS or an
already-open connection. Select the VFS in `sqlite3_open_v2` or the database URI.
[abi_smoke.c](../tests/build/abi_smoke.c) is a complete loading/opening example.

For a fresh database, configure one writable connection before opening readers
or creating schema:

```sql
PRAGMA page_size = 4096;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
```

The VFS reserves its required page bytes automatically. Configure auto-vacuum
before selecting WAL when needed. Existing files must already use the selected
protected format. Static hosts call `monstervfs_register()` after SQLite
initialization; declarations are in [monstervfs.h](../include/monstervfs.h).

Close connections normally. Finalize statements retained by `sqlite3_close_v2`
before shutting down SQLite. Final connection teardown waits for active
maintenance, and later opens resume it. Optional `monstervfs_shutdown()`
permanently stops maintenance and joins its thread; call it outside SQLite
callbacks. It is unnecessary for ordinary connection cleanup.

## Standard checks

```sh
tools/test
```

This runs both formats: Python tooling tests, Rust units and Clippy, static and
loadable ABI checks, SQL integration, maintenance, fault injection, corruption,
read-only behavior, process stress, and modeled power loss. `--build copy` or
`--build parity` selects one. Allow substantial time; this is not the small CI
smoke selection. Upstream SQLite and dangerous device tests are separate.

For a focused change, start with the relevant host below and unit/lint checks
for both formats. Private Rust tests live in `tests/unit/`, outside source files.

```sh
cargo fmt --all --check
cargo test --lib --no-default-features --features loadable,copy --target-dir target/copy/loadable
cargo clippy --tests --no-default-features --features loadable,copy --target-dir target/copy/loadable -- -D warnings
```

| Tool | Focus |
| --- | --- |
| `test-build`, `test-registration` | Host/API compatibility, loading, registration failure and concurrency. |
| `test-builds` | Both formats loaded together; rejection of the other format's files. |
| `test-lifecycle` | Close during repair, deferred/shared close, reopening, and SQLite reinitialization; `--both` loads both libraries. |
| `test-integration` | SQL, snapshots, processes, initialization, VACUUM, temporary spills, savepoints, and recovery. |
| `test-maintenance` | Persistent repair, concurrent page changes, failed repair, and checkpoint ownership. |
| `test-faults` | Injected native I/O errors, guard failures, cleanup, and failed checkpoint persistence. |
| `test-metadata` | Prefix/control damage, foreign identities, failed recovery, and offline copies. |
| `test-damage` | Correctable/unrecoverable contents, recovery cutoffs/refusals, snapshots, and scrubbing. |
| `test-readonly` | Native/protected permissions and read-only behavior; run as an ordinary user, not root. |
| `test-large` | SQLite's unused lock page, repair beyond it, and VACUUM; `--real` needs about 5 GiB of disk. |
| `test-soak` | Random damage checked against an external transaction history, followed by writes/checkpoints/reopens. |
| `test-stress` | Writers/readers/checkpointers across processes, kills, and damage. Default skips sync for throughput; `--synced` uses real barriers. |
| `test-crash` | Power-loss images reconstructed from SQLite I/O, including chained crashes and VACUUM. |

Focused tools use the selected format and build missing artifacts as documented
by their options. `test-build` expects `build-host --static` first. Build hosts
and fixtures sequentially: formats share extracted SQLite sources and parts of
the host output.

## Upstream SQLite tests

```sh
MONSTERVFS_BUILD=copy tools/build-testfixture
MONSTERVFS_BUILD=copy tools/test-sqlite
```

The fixture needs `make`, a Tcl 8.6+ shell, and matching development headers.
Use `--with-tclsh PATH` to select the shell and `--with-tcl DIRECTORY` for a
nonstandard directory containing `tclConfig.sh`. The tool does not install Tcl.
Repeat with `parity` for the other format.

The [applicability manifest](../tests/sqlite/applicability.json) defines selected,
lenient, broad, and excluded cases. `test-sqlite --broad` adds the broad list;
`--only-broad` runs that list alone. These are selections, not the entire SQLite
suite. `survey-sqlite [SCRIPT...]` evaluates additional candidates, with
`--vfs unix` providing a native comparison.

Lenient cases absorb unsupported pragma requests and keep WAL/FULL/4 KiB in
force; a script's requested VFS shim can also be bypassed. The upstream fixture
uses `SQLITE_NO_SYNC`. Passing it is SQL regression evidence, not proof of crash
durability or unsupported modes. Never enable `lenient-pragmas` in a release.

**Rebuild the fixture after source changes.** `test-sqlite` uses an existing
statically linked fixture. `test-upstream-stress` also needs the extracted sources;
it compares SQLite's thread/process workloads with the native VFS and requires
matching nonempty successful summaries.

## Recovery and performance experiments

Capture a release library when testing a fixed candidate:

```sh
export MONSTERVFS_BUILD=copy
LIB=$(tools/build-library)
tools/test-crash --every 1 --library "$LIB"
tools/test-soak --trials 3000 --seed 42 --library "$LIB" --keep /tmp/monstervfs-failures
tools/measure --repeat 3
```

`test-crash` preserves everything covered by each recorded sync, then varies
unflushed writes, lengths, deletions, and torn sectors. `--workload` selects
standard, hard, or VACUUM work; `--chains` and `--seeds` expand coverage.
Ordinary-crash cases require acknowledged commits to survive. Process kills in
`test-stress` do not model power loss, even with `--synced`.

`test-soak` distinguishes exact recovery, consistent prefix loss, read/open
errors, wrong data, and incomplete runs. Partial reads are not verified recovery.
Verified recoveries continue through a new transaction, checkpoint, and reopen.
`--keep` preserves failed inputs, external write history, and observations;
`--vfs unix` supplies a native comparison. Equal physical fault offsets in the
two formats need not damage equivalent logical contents.

`test-recovery-matrix --parity-library PATH --copy-library PATH --output DIR`
uses matched logical rows/transactions and preserves manifests and observations
in a new directory. Its accepted control-plus-frame double fault remains a
visible wrong-data outcome, not a successful recovery. See
[ARCH.md](../ARCH.md) for recovery limits. A native-control failure makes a
comparison inconclusive; it cannot excuse a protected failure.

`measure` reports elapsed time, write volume, sync calls, and file sizes for
native/protected WAL/FULL workloads. Use `--directory` to select a filesystem
and `--json` for structured output. Results describe that workload and host,
not a general speed guarantee. `measure-codec` isolates parity codec cost.

## Reproducible runs

Keep each run's source and libraries stable. `--library` and
`MONSTERVFS_TEST_LIBRARY` use an existing artifact without rebuilding it; verify
its identity rather than relying on timestamps. Record the revision and dirty
changes, artifact hashes, command, format, seed, host, and filesystem.

Long suites may run in the background with captured output and an explicit
completion/exit record. Preserve failures and inspect counts, skips, and native
controls as well as exit status. An unfinished run has no passing result.
Interrupted crash/upstream runs can leave `monstervfs-crash-*` or
`monstervfs-sqlite-*` temporary directories; inspect ownership before cleanup.

[Dangerous Linux device tests](danger/README.md) are never part of `tools/test`
and require separate approval and a disposable host. Their availability does
not establish that every filesystem or kernel has been qualified.
