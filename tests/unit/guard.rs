use super::*;
use std::{ffi::CString, sync::mpsc, time::Duration};
fn path() -> CString {
    let mut bytes = [0; 16];
    getrandom::fill(&mut bytes).unwrap();
    CString::new(format!(
        "/tmp/monstervfs-guard-{}-{:x}",
        std::process::id(),
        u128::from_le_bytes(bytes)
    ))
    .unwrap()
}
fn remove(path: &CStr) {
    unsafe {
        libc::unlink(path.as_ptr());
    }
}
#[test]
fn thread_access_is_serialized_even_for_shared_ofd_descriptor() {
    let path = path();
    let (a, new) = Lease::open(&path, true, 0o600, CANTOPEN).unwrap();
    assert!(new);
    let (b, new) = Lease::open(&path, true, 0o600, CANTOPEN).unwrap();
    assert!(!new);
    assert!(Arc::ptr_eq(&a.shared, &b.shared));
    let (start_tx, start_rx) = mpsc::channel();
    let (done_tx, done_rx) = mpsc::channel();
    let handle = a
        .access()
        .with(true, || {
            let handle = std::thread::spawn(move || {
                start_tx.send(()).unwrap();
                b.access()
                    .with(false, || {
                        done_tx.send(()).unwrap();
                        Ok(())
                    })
                    .unwrap();
            });
            start_rx.recv().unwrap();
            assert!(done_rx.recv_timeout(Duration::from_millis(30)).is_err());
            Ok(handle)
        })
        .unwrap();
    done_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    handle.join().unwrap();
    drop(a);
    remove(&path);
}
#[test]
fn process_lock_probe() {
    let Ok(path) = std::env::var("MONSTERVFS_GUARD_TEST_PATH") else {
        return;
    };
    let path = CString::new(path).unwrap();
    let (lease, _) = Lease::open(&path, false, 0o600, CANTOPEN).unwrap();
    let expected = std::env::var("MONSTERVFS_GUARD_EXPECT_BUSY").unwrap() == "1";
    let fd = lease.shared.fd.load(Ordering::Relaxed);
    let mut range: libc::flock = unsafe { std::mem::zeroed() };
    range.l_type = libc::F_WRLCK as _;
    range.l_whence = libc::SEEK_SET as _;
    range.l_start = 100;
    range.l_len = 1;
    let code = unsafe { libc::fcntl(fd, libc::F_SETLK, &range) };
    assert_eq!(code < 0, expected);
}
fn probe(path: &CStr, busy: bool) {
    let status = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "guard::tests::process_lock_probe"])
        .env("MONSTERVFS_GUARD_TEST_PATH", path.to_str().unwrap())
        .env("MONSTERVFS_GUARD_EXPECT_BUSY", if busy { "1" } else { "0" })
        .status()
        .unwrap();
    assert!(status.success());
}
#[test]
fn a_known_inode_needs_no_new_descriptor() {
    let path = path();
    let (first, new) = Lease::open(&path, true, 0o600, CANTOPEN).unwrap();
    assert!(new);
    let mut leases = Vec::new();
    for _ in 0..20 {
        leases.push(Lease::open(&path, true, 0o600, CANTOPEN).unwrap().0);
    }
    let registry = files().lock().unwrap();
    assert_eq!(registry.entries[&first.key].descriptors.len(), 1);
    assert_eq!(registry.entries[&first.key].users, 21);
    drop(registry);
    drop(leases);
    drop(first);
    remove(&path);
}
#[test]
fn created_files_use_the_requested_mode() {
    use std::os::unix::fs::PermissionsExt;
    let path = path();
    let previous = unsafe { libc::umask(0o022) };
    let (lease, _) = Lease::open(&path, true, 0o644, CANTOPEN).unwrap();
    unsafe { libc::umask(previous) };
    let metadata = std::fs::metadata(path.to_str().unwrap()).unwrap();
    assert_eq!(metadata.permissions().mode() & 0o777, 0o644);
    drop(lease);
    remove(&path);
}
#[test]
fn dropping_duplicate_descriptor_does_not_release_existing_posix_lock() {
    let path = path();
    let (a, _) = Lease::open(&path, true, 0o600, CANTOPEN).unwrap();
    let (b, _) = Lease::open(&path, true, 0o600, CANTOPEN).unwrap();
    let mut range: libc::flock = unsafe { std::mem::zeroed() };
    range.l_type = libc::F_WRLCK as _;
    range.l_whence = libc::SEEK_SET as _;
    range.l_start = 100;
    range.l_len = 1;
    assert_eq!(
        unsafe { libc::fcntl(a.shared.fd.load(Ordering::Relaxed), libc::F_SETLK, &range) },
        0
    );
    probe(&path, true);
    drop(a);
    probe(&path, true);
    drop(b);
    probe(&path, false);
    remove(&path);
}
#[test]
fn a_file_this_process_cannot_write_still_takes_the_shared_guard() {
    let path = path();
    drop(Lease::open(&path, true, 0o400, CANTOPEN).unwrap());
    let (lease, created) = Lease::open(&path, false, 0, CANTOPEN).unwrap();
    assert!(!created);
    lease.access().with(false, || Ok(())).unwrap();
    drop(lease);
    remove(&path);
}
#[test]
fn a_directory_that_refuses_a_new_file_reports_the_requested_code() {
    let directory = path();
    assert_eq!(unsafe { libc::mkdir(directory.as_ptr(), 0o500) }, 0);
    let file = CString::new(format!("{}/file", directory.to_str().unwrap())).unwrap();
    let error = Lease::open(&file, true, 0o600, 1544).err().unwrap();
    assert_eq!(error.code, 1544);
    assert_eq!(unsafe { libc::rmdir(directory.as_ptr()) }, 0);
}
fn mode(path: &CStr, mode: libc::mode_t) {
    assert_eq!(unsafe { libc::chmod(path.as_ptr(), mode) }, 0);
}
fn descriptors(lease: &Lease) -> usize {
    files().lock().unwrap().entries[&lease.key]
        .descriptors
        .len()
}
#[test]
fn the_guard_moves_to_a_writable_descriptor_once_the_file_allows_one() {
    let path = path();
    drop(Lease::open(&path, true, 0o400, CANTOPEN).unwrap());
    let (reader, _) = Lease::open(&path, false, 0, CANTOPEN).unwrap();
    mode(&path, 0o600);
    let (writer, created) = Lease::open(&path, false, 0, CANTOPEN).unwrap();
    assert!(!created);
    writer.access().with(true, || Ok(())).unwrap();
    reader.access().with(true, || Ok(())).unwrap();
    // The old descriptor stays: closing it could release a POSIX lock on this inode.
    assert_eq!(descriptors(&reader), 2);
    let (later, _) = Lease::open(&path, false, 0, CANTOPEN).unwrap();
    assert_eq!(descriptors(&reader), 2);
    drop((reader, writer, later));
    remove(&path);
}
#[test]
fn a_file_that_stays_unwritable_adds_no_descriptors() {
    let path = path();
    drop(Lease::open(&path, true, 0o400, CANTOPEN).unwrap());
    let (first, _) = Lease::open(&path, false, 0, CANTOPEN).unwrap();
    let before = descriptors(&first);
    let others: Vec<_> = (0..5)
        .map(|_| Lease::open(&path, false, 0, CANTOPEN).unwrap().0)
        .collect();
    // Root can open anything, so one writable descriptor is all it can add.
    assert!(descriptors(&first) <= before + 1);
    drop(others);
    drop(first);
    remove(&path);
}
#[test]
fn moving_the_guard_waits_for_whoever_holds_it() {
    let path = path();
    drop(Lease::open(&path, true, 0o400, CANTOPEN).unwrap());
    let (reader, _) = Lease::open(&path, false, 0, CANTOPEN).unwrap();
    let access = reader.access();
    let (held, wait) = mpsc::channel();
    let (finish, finished) = mpsc::channel::<()>();
    let holder = std::thread::spawn(move || {
        access
            .with(false, || {
                held.send(()).unwrap();
                finished.recv().unwrap();
                Ok(())
            })
            .unwrap();
    });
    wait.recv().unwrap();
    mode(&path, 0o600);
    let copy = path.clone();
    let (opened, done) = mpsc::channel();
    let opener = std::thread::spawn(move || {
        let lease = Lease::open(&copy, false, 0, CANTOPEN).unwrap().0;
        opened.send(()).unwrap();
        lease
    });
    // Root started with a writable descriptor, so only others have anything to wait for.
    if unsafe { libc::geteuid() } != 0 {
        assert!(done.recv_timeout(Duration::from_millis(100)).is_err());
    }
    finish.send(()).unwrap();
    holder.join().unwrap();
    let writer = opener.join().unwrap();
    writer.access().with(true, || Ok(())).unwrap();
    drop((reader, writer));
    remove(&path);
}
#[test]
fn a_direct_read_returns_exact_bytes_and_leaves_the_descriptor_as_it_was() {
    let path = path();
    let (lease, _) = Lease::open(&path, true, 0o600, CANTOPEN).unwrap();
    let fd = lease.shared.fd.load(Ordering::Relaxed);
    let data: Vec<u8> = (0..20_000u32).map(|i| (i * 7 + i / 251) as u8).collect();
    assert_eq!(
        unsafe { libc::pwrite(fd, data.as_ptr().cast(), data.len(), 0) },
        data.len() as isize
    );
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    // Some filesystems refuse direct reads, and those simply keep the original error.
    if set_direct(fd, true) {
        set_direct(fd, false);
        for (offset, length) in [(0, 4096), (4096, 8192), (100, 1), (4095, 2), (19_000, 1000)] {
            let mut bytes = vec![0; length];
            assert!(lease.read_direct(offset as u64, &mut bytes));
            assert_eq!(bytes, data[offset..offset + length]);
        }
        assert!(!lease.read_direct(19_999, &mut [0; 2]));
    }
    assert_eq!(unsafe { libc::fcntl(fd, libc::F_GETFL) }, flags);
    remove(&path);
}
