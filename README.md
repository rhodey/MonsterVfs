# MonsterVfs

Two SQLite VFS that target `journal_mode=WAL` and `synchronous=FULL` and
self-repair. These VFS make sense for single-disk systems where RAID is not
available. These VFS complement [MonsterFt](https://github.com/rhodey/monsterft)
but have value on their own.

VFS **monstervfs_copy** achieves self-repair by storing two copies of every SQLite
page in adjacent sectors. VFS **monstervfs_parity** achieves self-repair by storing
one copy of every SQLite page plus [parity bytes](https://en.wikipedia.org/wiki/Parity_bit)
in adjacent sectors. The copy VFS assumes loss of full disk sectors is most common.
The parity VFS assumes corruption within disk sectors is most common. Both cost 2x
storage.

MonsterVfs is 99% Rust, any C and Python is bindings and test harnesses.

## Tradeoffs

| Damage | Copy | Parity |
| ------ | ---- | ------ |
| Sector 1 lost | Survives | Survives |
| Sector 2 lost | Survives | Lost |
| Few bad bytes in sector 1 | Survives | Survives |
| Few bad bytes in sector 2 | Survives | Survives |
| Few bad bytes in both sectors | Lost | Survives |
| Many bad bytes in sector 1 | Survives | Survives |
| Many bad bytes in sector 2 | Survives | Lost |
| Many bad bytes in both sectors | Lost | Lost |

| Speed penalty | Default | Copy | Parity |
| ------------- | ------- | ---- | ------ |
| Small commits | 1x | 1.0x | 1.1x |
| Larger batch | 1x | 1.1x | 3.7x |
| Checkpoint | 1x | 1.7x | 4.0x |

## Maintenance

Repair is queued for damage discovered at read time but there is no
[scrubbing](https://en.wikipedia.org/wiki/Data_scrubbing) schedule. Use a
normal SQLite connection for scrubbing, a read-only connection never queues
repairs. From your app do this:

```sql
SELECT data FROM sqlite_dbpage WHERE pgno >= 0 AND pgno < 42;
```

Table `sqlite_dbpage` comes from `SQLITE_ENABLE_DBPAGE_VTAB` and as described
earlier reading queues repair so your app just needs to step through ranges on
a schedule. Repair uses a background thread plus an SQLite connection so your
app will see `SQLITE_BUSY` sometimes unless you set a good busy timeout.

## Limitations

+ `ATTACH` not supported
+ `VACUUM INTO` not supported
+ SQLite `SQLITE_ENABLE_DBPAGE_VTAB` needed
+ SQLite `SQLITE_CONFIG_SINGLETHREAD` not supported
+ Tested on Linux and macOS: ext4, btrfs, XFS, APFS
+ Version = 0.5.0 => Not yet suggested for production

## Build

```sh
MONSTERVFS_BUILD=copy tools/build-library
MONSTERVFS_BUILD=parity tools/build-library
```

Outputs `libmonstervfs_copy.so` and `libmonstervfs_parity.so` on linux, `.dylib`
on macOS.

```sh
cargo build --release --no-default-features --features static,copy --target-dir target/copy/static
cargo build --release --no-default-features --features static,parity --target-dir target/parity/static
```

Outputs `libmonstervfs.a` to `target/copy/static/release/` or
`target/parity/static/release/`.

## Roadmap

As with [MonsterFt](https://github.com/rhodey/monsterft) this repo will be
aiming for a 1.0 release. 0.5 proved some ideas that helped validate the
direction MonsterFt is headed in for online and offline repair.

## License

The work is licensed `AGPL-3.0-only` to encourage collaborations while
leaving open a path for negotiating commercial licenses and support
packages. Write to hello@lock.host.

### Copyright

LOCK HOST, INC. 2026.
