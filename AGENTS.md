# Agent guide

MonsterVfs provides two Rust SQLite VFS builds: `parity` and `copy`. Start with
[README.md](README.md) for the introduction and [ARCH.md](ARCH.md) for supported
behavior, format/layout, source responsibilities, and recovery limitations.
Read the relevant architecture section and callers before changing a code path.

[tools/README.md](tools/README.md) explains building, loading, test selection, and
interpreting results. [CONTRIBUTING.md](CONTRIBUTING.md) covers contributions.

## Code and tests

- Rust implementation lives in `src/`; `c/` provides the SQLite API bridge.
  Use ARCH's source map to find the owner of a behavior.
- Keep test bodies under `tests/`, outside implementation files. Private unit
  tests use `#[path]` modules; integration tests use independent SQLite hosts.
- Prefer small changes with explicit ownership and error propagation. For I/O,
  FFI, locking, or cleanup changes, inspect success, failure, and reopening paths.
- Check both builds when shared code changes. Keep format/policy changes explicit
  and preserve existing regression tests unless the supported contract changes.
- Preserve vendored source, licenses, notices, and provenance. Generated bindings
  are checked in; use `tools/generate-bindings` when the C API changes.

## Verification

Start with relevant tests, formatting, and lint; use the tool guide to select
broader coverage. `cargo test --lib` selects parity by default. For example:

```sh
cargo fmt --all --check
cargo test --lib --no-default-features --features loadable,copy --target-dir target/copy/loadable
cargo clippy --tests --no-default-features --features loadable,copy --target-dir target/copy/loadable -- -D warnings
```

Long runs may execute in the background. Keep their source/artifacts stable and
record commands, hashes, seeds, platform, logs, and completion status. Read actual
failures and skips. Rebuild static test fixtures after source changes.

The [dangerous device tests](tools/danger/README.md) need explicit authorization
and a disposable Linux host. Never include them implicitly in routine testing.

## Documentation

Keep README introductory. Put behavior and limitations in ARCH, and test commands
and scope in the tool guides. Link to those documents instead of repeating them.
Describe what changed and the evidence supporting it; distinguish reproduced bugs,
accepted limits, hypotheses, and missing coverage.
