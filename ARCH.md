# Architecture

MonsterVfs synchronously maps SQLite files to protected physical slots through
the native Unix VFS. SQLite owns transactions, WAL, snapshots, checkpoints, and
logical locks. Checksums and redundancy add recovery from damaged database pages
and WAL frames while retaining SQLite's WAL/FULL durability rules. Targets are
Linux and macOS on local filesystems with SQLite WAL locking/shared-memory support.

[README.md](README.md) introduces the project. This document owns the supported
contract, implementation map, and limitations; [tools/README.md](tools/README.md)
covers building and validation. Version 0.5.0 is not production-ready.

## Host and registration

Select one linkage feature (`loadable` or `static`) and one codec (`parity` or
`copy`); defaults are `loadable,parity`. The codecs register `monstervfs_parity`
and `monstervfs_copy`. Each loadable library exports the standard
`sqlite3_extension_init` entry point, so loading needs no explicit entry name.
Dynamic builds coexist; static builds share C symbols, so a host links one codec.
Public controls and static entry points are in
[include/monstervfs.h](include/monstervfs.h).

The host must match the SQLite version and exact source ID pinned in
[c/sqlite_api.c](c/sqlite_api.c), enable `SQLITE_ENABLE_DBPAGE_VTAB`, and support
threaded connections. Global single-thread mode and network filesystems are
unsupported; local filesystems must support SQLite's Unix locking and WAL SHM. Loadable builds
use the host API without an embedded SQLite engine. Dependencies, ABI bindings,
and libfec are pinned; only `parity` links libfec.

Registration captures the native `unix` VFS and immutable API pointers. The VFS
and worker stay private until reserve-byte autoextension registration succeeds;
failure joins the worker before unloading is possible. Automatic callbacks
tolerate the brief unpublished interval. The initialized pinned host's final VFS
publication is infallible; an unexpected error aborts rather than unloading live
callbacks. Successful loadable registration returns `SQLITE_OK_LOAD_PERMANENTLY`.

Persistent operation targets WAL/FULL. Writable handles reject explicit unsupported
page/journal/sync settings, but startup rollback is not forcibly changed to WAL.
An autoextension requests 48 reserved bytes for the protected main schema before initialization; the request
itself creates no content. Set auto-vacuum before WAL on a fresh database, or use
VACUUM to change it later. Ordinary VACUUM's internal attachment is supported;
application `ATTACH`/`DETACH` and `VACUUM INTO` are unsupported usage, without a
separate rejection hook. The protected formats are not native SQLite files and
are not interchangeable. Import native content through SQL into a fresh protected
database; backup/VACUUM tests do not establish general conversion compatibility.
Initialize through a writable connection before opening readers. Keep main and
any required WAL together when copying a closed database.

## Source map

| Source | Responsibility |
| --- | --- |
| `src/lib.rs`, `src/config.rs` | Features, registration entry points, allowed settings. |
| `src/error.rs` | SQLite errors and unreadable-data classification. |
| `src/format.rs` | File profiles, slot geometry, offsets, logical EOF. |
| `src/codec.rs` | Shared headers, controls, hashes, temporary slots. |
| `src/parity.rs`, `src/fec.rs` | Parity records/slots and libfec ownership. |
| `src/copy.rs` | Duplicated records and pages. |
| `src/file.rs` | Logical I/O, pending WAL headers, controls, truncation. |
| `src/guard.rs`, `src/native.rs` | Physical exclusion, descriptors, native I/O/retries. |
| `src/recovery.rs` | WAL scan, cut/refuse decisions, loss evidence. |
| `src/maintenance.rs` | Repair observations, worker connections, status. |
| `src/vfs.rs` | SQLite callbacks, handle ownership, recovery hooks. |
| `src/ffi.rs`, `src/ffi_bindings.rs`, `c/sqlite_api.c` | Host dispatch and ABI. |

`vendor/` retains dependencies and provenance. Test bodies live under `tests/`,
including private unit tests included through `#[path]`. `tools/` builds
independent hosts and drives validation. SQLite upgrades require review of the
host ABI, callback ordering, and wal-index assumptions.

## Physical format

### File classes and geometry

Pages and alignment are fixed at 4096 bytes; native sector size must not exceed
that. Every slot is 8192 bytes, including temporary slots and all parity settings.
`parity` accepts even parity sizes 32–1024 bytes (default 32); an explicit
`mv_parity` must match an existing file. `copy` rejects it. Both reject `mv_block`,
`mv_align`, and verified unsupported geometry without authorizing replacement.
The codecs reject each other's recognized files. Format version 1 remains
pre-release with no migration promise.

| SQLite file class | Storage |
| --- | --- |
| `MAIN_DB` | Protected page slots after an 8192-byte prefix. |
| `WAL` | Protected frame slots after prefix and 8192-byte controls. |
| `TEMP_DB`, `TEMP_JOURNAL`, `TRANSIENT_DB`, `SUBJOURNAL` | Checksum-only slots with private lifetime. |
| `MAIN_JOURNAL` | Native initialization/hot-journal bytes and behavior. |
| Shared memory | Native, rebuildable WAL index. |

Other `xOpen` file classes are rejected. Native initialization journals retain
SQLite's sync, deletion, and read-only probing behavior; close does not
unconditionally delete them.

### Prefixes and controls

Two immutable-header regions begin at offsets 0 and 4096. Their 128-byte decoded
payload records magic, version, kind, random 16-byte identity, settings, and
reserved fields. Device/inode values are runtime coordination keys only;
consistent offline copies retain their identity across paths and devices.

In `parity`, a region holds a 256-byte record: data, three data hashes, 32 parity
bytes, and three parity hashes. One usable region suffices; two must agree.
In `copy`, a 4096-byte record repeats data and its hash at offsets 0 and 2048.
Unanimous surviving halves suffice, even one. Conflicting valid halves require
a strict majority and an intact agreeing region to anchor repair. Ambiguity
returns `SQLITE_CORRUPT` and cannot authorize reinitialization.

WAL controls occupy regions at offsets 8192 and 12288; frame slots start at
16384. Each control contains a sequence, SQLite's 32-byte WAL header, backfill
salts/count, and immutable WAL identity in protected payload bytes 60–75.
The selected codec protects records; writes alternate regions. Selection examines
every independently verified candidate, including both `copy` halves, and
chooses the newest sequence with a valid SQLite header. Different contents at
that selected sequence, or any structurally valid verified foreign identity,
return `SQLITE_CORRUPT`. Controls are recovery metadata, not a transaction log.

### Slots and verification

Persistent pages reserve their final 48 bytes: hash16, initialized-length4,
WAL-frame-header24 (zeros for main), spare/copy-number4. SQLite reads zeros there;
nonzero incoming reserved bytes are rejected. Page-one size/reserve fields are
validated. There is no separate protected copy of SQLite's first 100 header bytes.

Slot `k` begins at `C + k*8192`, with `C=8192` for main/temp and `C=16384` for WAL.
Main/temp logical payloads are 4096 bytes; WAL payloads are 4120 bytes, including
the 24-byte frame header. WAL logical content begins at 32; its header comes from
the control record.

Parity slots begin with metadata (identity16, relative logical offset8,
initialized-length4, zeros4), three spaced data/parity-hash pairs, and parity;
the payload ends the slot. Metadata and the zero-expanded payload are protected.
The complete page's trailer can independently verify/reconstruct the protected
data when the metadata sector is lost. Independently proven conflicting writes
are rejected before parity can turn one valid version into another.

Copy slots hold two 4096-byte pages with trailers. Each hash binds file identity,
relative offset, initialized length, WAL header, and page content before the
reserved area. Copy numbers differ and are excluded from the hash; the WAL header
lives in each trailer. One valid copy suffices and marks degradation; two valid
different copies or no valid copy are errors. This differs from prefix voting and
control sequence selection. Persistent copy slots are always written whole.

Temporary slots use metadata, three data hashes, padding, and payload, without
content correction or trailers. Their immutable prefix uses the selected codec.

Hashes are canonical big-endian XXH3-128 values, not authentication or freshness
proof. Parity accepts a match against any stored hash copy; damaged protection
beside verified data marks degradation. Shortened systematic Reed–Solomon uses
little-endian 16-bit symbols, `GF(2^16)`, polynomial `0x1100b`, generator 2,
first root 0, root step 1. Default parity corrects eight unknown damaged symbols.
Reconstruction must match a pre-existing data hash; no located erasures are
supplied to libfec.

All present bytes must be zero before a main/temp slot is accepted as a sparse
hole. Failed reads cannot invent that evidence. Zero WAL slots are damage.

### EOF and initialization

For parity/temp, let `D` be payload size, `P=8192-D`, and
`Ldata=q*D+r`, excluding the WAL header. Physical length is
`C + q*8192 + (r>0 ? P+r : 0)`. Reverse mapping ignores tails wholly within
protection space. Persistent copy mapping ignores incomplete physical tails.
A WAL with no payload reports logical length zero. Checked arithmetic bounds
offsets by signed 64-bit I/O limits. Logical short reads zero-fill the remainder
and return `SQLITE_IOERR_SHORT_READ`; unexpected physical short reads remain errors.

Initialized length defines an interior partial block's zero suffix. Partial
updates verify retained bytes; full replacements do not read old contents.
Partial truncation rewrites the retained record with a zero suffix before
truncating, so extension cannot resurrect old bytes. Main checkpoint truncation
is page-aligned. WAL trimming rounds down to a whole frame without reading a
damaged dead frame, including `journal_size_limit` trims.

Persistent creation writes and syncs the prefix before extending content;
temporary creation skips that durability barrier. Under an exclusive guard,
writable open can recreate unusable prefix-only files (up to 8 KiB main/temp or
16 KiB WAL), excluding recognized native/other-codec files and verified conflicts
or unsupported geometry. This can overwrite an unrelated small file with an
unrecognized prefix; it is not a general-purpose file-type safety check.

Read-only opens never initialize. Disposable main readers must close before
writable initialization: later read/size callbacks reject a retained unfinished
main handle with `SQLITE_CANTOPEN`, though SQLite cache hits may bypass callbacks.
Database registration is fixed at open. Read-only WAL adoption remains supported
for an already initialized main database.

## I/O and ownership

`vfs.rs` owns stable SQLite wrappers and main/WAL association; `native.rs` owns
the delegated handle and closes it once, including methods installed on failed
open. Delegation preserves `sqlite3_filename` and its URI/related-file data.

`guard.rs` shares an OFD descriptor and local mutex per device/inode. A byte-range
lock at offset 8191 covers physical reads, sizes, modifications, truncation, and
retries: shared for reads, exclusive for mutations. The mutex serializes local
users of one OFD. SQLite's transaction locks remain separate and delegated.

Closing an extra descriptor can drop another local handle's process-owned POSIX
locks. The registry retains required descriptors through final native close;
writable upgrades retain old descriptors. Temporary creation obtains the guard
before native delete-on-close unlinks the path, without repeating exclusive
creation. Native allocation precedes file creation. Failed native open removes
a newly created delete-on-close path only best-effort and only if `lstat` still
matches the held guard identity; the original open error survives.

Acquire SQLite protocol exclusion before the short I/O guard; never run SQL or
upgrade SQLite locks under it. Ordinary `xSync` runs outside it; creation and
prefix repair retain exclusion across durability barriers. Guards provide runtime
coherence, not power-loss atomicity. macOS sync requests use full-sync flags;
new files also sync their directory before clearing pending directory durability.

Native transfers are split at 64 KiB. Unreadable buffered reads retry through
the guard descriptor using Linux `O_DIRECT` or macOS `F_NOCACHE`; slot decoding
can then read self-contained pages/copies separately. Other errors keep their
semantics.

A pending 24-byte WAL frame header joins its following page write into one slot
write. Reads, sizes, syncs, truncates, close, and unrelated writes flush it.
Failed SQLite-facing writes/flushes discard the pending header, including guard
failures, before SQLite can release the writer lock and another writer reuse the
frame. Independent prefix-maintenance failures leave it alone. Private-index
tracking depends on SQLite's paired header/page writes; arbitrary rechunking
shims are outside the validated contract.

Protected `xSectorSize` reports 4096. Persistent main/WAL advertise powersafe overwrite
because each page/frame owns its slot; temporary files advertise no capabilities.
No other atomic-write capability is claimed. Mapped reads are disabled, `nolock`
is rejected, and size/chunk hints are no-ops. Safe native controls are explicitly
forwarded; `SYNC`/`COMMIT_PHASETWO` add no transaction protocol.

Failed writes permit SQLite rollback/retry. Failed protected truncate or non-main
sync stops the handle; main sync failure is reported/counted but permits reads
and checkpoint retry with WAL retained. Watermark failures stop main because
SQLite ignores the checkpoint-start result. Recovery failures stay latched so
close-time checkpointing cannot erase evidence. Unlock/close remain available.
Panics cannot unwind into SQLite. Unreadable-data errors are logged through
`sqlite3_log` and normally use `SQLITE_IOERR_DATA`;
`SQLITE_IOERR_CORRUPTFS` after native read retries also enters the unreadable-data
policy, unlike arbitrary I/O errors.

## Recovery intro

In total MonsterVfs both copy and parity VFS exceed SQLite default VFS in total
number of recoverable crashes and total number of repairable damage cases. When
you start to list out all the ways that WAL mode can encounter damage and what
damage can lead to the result can be stressful. Nonetheless it is right that
this document do so.

MonsterVfs paired with MonsterFt is a winning combination because under no
condition will an application result ever reach a application client unless a
quorum of nodes agrees on the SQLite changeset.

## Recovery

WAL/FULL requires acknowledged commits to survive ordinary crashes when the
underlying filesystem honors sync. MonsterVfs keeps this requirement for both
codecs; crash tests verify it against native SQLite controls.

Later corruption is a separate case. [Native SQLite WAL recovery](https://www.sqlite.org/walformat.html)
stops at an invalid checksum and selects the last valid commit in the preceding
frames. Damage to previously durable frames can therefore lose acknowledged
commits with the native VFS too. MonsterVfs first corrects damage using its
redundancy, preserving transactions that native recovery would otherwise lose.
If correction cannot supply valid frames, recovery uses the cutoff/refusal
rules below.

An unreadable initial main read of at most 100 bytes may return a minimal SQLite
header with the configured page/reserve sizes so SQLite can reach WAL recovery.
Full-page reads still require verified contents.

Before the first checkpoint main write, `SQLITE_FCNTL_CKPT_START` records WAL salts
and `nBackfillAttempted` in the alternating control region and syncs WAL. This
watermark describes frames that may have reached main, not checkpoint completion.
With a private exclusive-mode wal-index, opening the WAL follows SQLite's
salts/checksum chain to its last commit; subsequent completed commit-frame writes
update that bound and a new header resets it. Physical slot count is not a bound.

An unreadable control region can be skipped only with no physical frames, or a
first frame that fully verifies without lone-copy reconstruction and matches
the surviving control's salts. A checksum-invalid record can be an interrupted
write and is skipped. Frames without a usable control require refusal.

The scanner verifies/corrects frames in memory without rewriting them. Zero,
incomplete, or unreadable frames enter the damage decision. A fully verified
old-generation frame normally marks reused tail space; lone-copy reconstruction
or evidence of a later completed commit makes it damage instead.

Damage below the matching backfill watermark, or nonempty WAL EOF below it,
returns `MONSTERVFS_CORRUPT_BACKFILLED` (extended `SQLITE_CORRUPT`). This guards
against mixing transactions already partly copied to main with an older WAL
prefix, a failure also reproduced with native SQLite after WAL corruption.
A frame-free WAL is allowed as completed truncate-checkpoint state. Otherwise,
recovery may discard the damaged suffix and let SQLite select the last valid
retained commit, with the corruption-related loss described above.

Two limits remain: losing the newest watermark together with damage below that
lost watermark can defeat this guard and expose mixed transactions. Also, the
conservative watermark can refuse an intact main after a completed checkpoint;
there is no durable finished-checkpoint flag. Refusal survives close and requires
external repair. These concern damage recovery, not ordinary-crash durability.

Writable cutoff is authorized at two hooks:

1. After native exclusive `WAL_RECOVER_LOCK` (index 2), before SQLite reads size.
   A required cut additionally takes all five WAL read locks exclusively and
   nonblocking; active snapshots return BUSY without a cut. Main EXCLUSIVE must
   not be requested here because idle connections retain SHARED.
2. WAL open when its associated main already owns SQLite's native EXCLUSIVE lock.
   The wrapper must not release that lock.

Truncate and fully sync before exposing the new size. Release newly acquired
locks when preparation fails; SQLite will not release a lock reported unacquired.
Do not rewrite/delete SHM to force recovery. Later exclusive paths bypassing
these hooks retain strict reads and may require reopening.

Read-only WAL or SHM uses the same scan from `xFileSize` to expose a virtual cutoff
without changing bytes; it rescans across transactions and preserves refusals.
A later writer can make the physical cut. Live reads never return unchecked WAL.
Cutoffs/refusals are logged and counted locally. Only a following frame continuing
a commit's checksum chain proves that discarded commit completed; the last commit
can resemble a torn tail. Counts are incomplete and do not survive restart.

## Maintenance

Read-time correction is in memory. Writable application main reads may queue
identities/fingerprints, never page payloads; read-only handles request no
persistent repairs. The worker opens its own host connection read/write without
CREATE, with private cache, verifies identity, and rewrites the current page in
`BEGIN IMMEDIATE` using:

```sql
UPDATE sqlite_dbpage SET data=data WHERE schema='main' AND pgno=?;
```

The worker enables dbpage writes only on its own connection. It commits via WAL/FULL
and disables automatic/close-time checkpoints; applications own checkpoint timing.
A committed replacement is distinct from physical retirement: snapshots and
retained tails can still reach old damage. Fingerprint deduplication prevents
repeated identical repair commits.

Implementation limits in `maintenance.rs` are 4096 queued identities, 16-page
batches, a one-shot 25 ms batching window, bounded retry/backoff, and 300 s prefix
checks. Later observations do not extend the window; application writes do not
defer it. Only a multi-page `SQLITE_IOERR_DATA` failure triggers individual
attempts, retaining cancellation and per-page outcomes. Backend cleanup failure
prevents further backend calls. BUSY/exclusive locking may delay repair.

Last writable application close cancels queued work; readers and worker handles
do not sustain authorization. Logical connection tokens count shared-cache
connections separately and remain alive through deferred `sqlite3_close_v2`
teardown. On connection teardown, a private SQL-function destructor releases
the token after SQLite has closed its Btrees. Failed function registration does
not activate the token; replacing that internal function withdraws that
connection's repair authorization.
The internal state query checks VFS ownership, including through a forwarding
shim, before exposing private state across separately loaded codecs.

The service starts work only while application connections exist. Last-connection
teardown waits for any active prefix check or private repair connection to finish;
a concurrently opened connection does not extend that wait. The worker then parks
without calling SQLite, and later connections resume maintenance. Status counts
all application connections; repair ownership counts writable ones. Observation
revisions/tokens prevent stale completion from clearing newer damage.

Prefix repair rereads under the exclusive guard, preserves a usable region, and
fully syncs its replacement before touching the survivor. Checks skip unavailable
handles, use another registered writable handle when possible, and stop on a
new I/O error. Healthy checks do not write. This work requires no SQL transaction
and must preserve logical contents and identity.

Applications scrub cold pages through fresh short-lived writable connections;
cache hits or newer WAL versions may hide main-file damage. Keep a writable
application connection alive for queued repair. Status via
`PRAGMA monstervfs_maintenance`/`MONSTERVFS_FCNTL_STATUS` is process-local;
`MONSTERVFS_FCNTL_ID` returns identity. Reading status starts no SQL repair.
Ordinary connection teardown needs no VFS shutdown call. After all SQLite
connections and deferred statements have closed, the host may call
`sqlite3_shutdown()` under SQLite's ordinary rules. Optional
`monstervfs_shutdown()` cancels work, joins the parked worker, and permanently
stops maintenance for that registration; call it outside SQLite callbacks.

## Protection, tradeoffs, and validation

The matched recovery tests demonstrate the added protection: single-bit damage
in checkpointed main data is corrected where native SQLite returns wrong data;
single-bit damage in a WAL payload is corrected where native recovery loses the
transaction suffix. When WAL damage exceeds correction below a known checkpoint
watermark, MonsterVfs reports corruption where native recovery can mix transaction
states. These cases are reproducible with `tools/test-recovery-matrix`.

The protection has defined limits and costs:

- Checksums verify contents, not freshness. Valid older images, lost writes,
  main-file shortening, and a stale copy whose counterpart is damaged may escape
  detection. A completely erased main slot is indistinguishable from a sparse
  hole; damaged holes have no stored protection from which to reconstruct zeros.
  Offline copies retain file identity.
- Correction covers the selected codec's redundancy. Damage before hashing,
  filesystem namespace/allocation damage, and loss of an entire slot are outside
  it. Both copies share a device. Default parity corrects scattered damage but
  cannot reconstruct a whole lost payload sector; a verified page trailer can
  recover a lost metadata sector. SHM and initialization journals retain native
  protection; temporary payloads have checksums without correction.
- SQLite's locking and access rules still apply. Active snapshots may delay a
  recovery cut, and read-only connections cannot initialize or recover a hot
  journal. Unsupported geometry and unrecoverable metadata require intervention.
  Applications manage cold-page scrubbing and external replica repair.
- Slots cost twice the page space, plus prefixes/controls and 48 reserved bytes
  per page. Each protected file needs a shared guard descriptor. Checkpoints add
  a watermark sync; initialization and prefix repair also have durability barriers.
  Recovery scans WAL, including repeat scans for read-only recovery. Maintenance
  uses SQLite's write lock; checkpoints retire replaced physical pages.

Validation includes standard integration, modeled crashes, selected upstream
SQLite tests, native comparisons, and memory/fault checks. Completed captures
cover macOS/APFS and Linux/ext4; earlier testing also exercised XFS and btrfs,
with a fresh cycle queued for both. The tool guide describes commands, coverage,
and the distinction between synced and no-sync workloads.
