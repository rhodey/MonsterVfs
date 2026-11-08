# Device tests that can hang the host

These Linux tools inject failures beneath real mounted filesystems. They need
root and `--may-hang-this-host`; `tools/test` never runs them. Obtain explicit
approval for each run and use a disposable machine whose reboot and data loss
are acceptable. The flag is an acknowledgement of risk, not authorization to
use someone else's host. Do not run these tools concurrently.

| Tool | Focus |
| --- | --- |
| `test-blocklog` | Record filesystem block writes, replay crash points, and verify the recovered database. |
| `test-powercut` | Interrupt live commits through device-mapper, remount, and check acknowledged transactions. |
| `test-faults-real` | Compare native/protected behavior under read-only mounts, full disks, and unreadable sectors. |

## Preparation and operation

Build as an ordinary user, selecting the same format for building and running:

```sh
export MONSTERVFS_BUILD=copy
tools/build-host
tools/smoke
sudo --preserve-env=MONSTERVFS_BUILD tools/danger/test-blocklog --may-hang-this-host
sudo --preserve-env=MONSTERVFS_BUILD tools/danger/test-faults-real --may-hang-this-host
sudo --preserve-env=MONSTERVFS_BUILD tools/danger/test-powercut --may-hang-this-host --seconds 300
```

Substitute `parity` for the other format. The tools need the system's filesystem
utilities, loop devices, and device-mapper modules. They invoke `mkfs`, `mount`,
`losetup`, `dmsetup`, and `modprobe` on image-backed devices they create.
`test-blocklog` needs about 3 GiB in `/var/tmp` plus space in `/dev/shm`.

`test-blocklog` and `test-powercut` accept `--fs`, `--options`, and `--library`.
Filesystem choices include ext4, XFS, and btrfs; a supported option is not a
qualification claim. `test-blocklog --vfs unix` is a native control.
`test-faults-real` uses the selected format's default layout and fixed fault
locations; it is not a general geometry test.

## Failure and cleanup

Injected errors can hang the kernel and unrelated sync operations. In particular,
ext4 `data=journal` error injection has required a forced reboot. The powercut
tool uses dropped writes in that mode, but this does not establish that a run
is safe. A forced reboot can lose unsynced data elsewhere on the host.

Interrupted runs can leave:

- Mounts under `/mnt/monstervfs-*` and loop devices.
- Device-mapper entries `monstervfs-blocklog`, `monstervfs-powercut`, or `monstervfs-faults`.
- Image directories under `/var/tmp/monstervfs-*` and `/dev/shm/monstervfs-blocklog.img`.
- Failed databases in `/var/tmp/monstervfs-powercut-failures` (configurable with `--keep`).

Inspect mounts, device-mapper entries, and loop-device backing paths before
removing only the run's resources. Do not blindly run another test over leftover
state or reboot a shared machine without its owner's approval.

Check workload/crash counts and retained failures, not just the final message.
Blocklog requires a completed workload and successful recovery checks. Powercut
requires verified cuts and a native `synchronous=OFF` sanity case that actually
loses data; skipping that control weakens the result. Real-fault cases include
contents after failed checkpoint, close, device restoration, and reopen.
Record format, artifact hashes, kernel, filesystem, mount options, seed, and
command with the results. [ARCH.md](../../ARCH.md) describes the recovery contract.
