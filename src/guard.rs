//! Short physical-I/O exclusion, independent of SQLite transaction locks.
use crate::{
    error::{CANTOPEN, Error, IOERR, IOERR_FSTAT, IOERR_LOCK, Result, zeros},
    format::HEADER_SIZE,
};
use std::{
    collections::HashMap,
    ffi::CStr,
    os::fd::{AsRawFd, FromRawFd, OwnedFd},
    sync::{
        Arc, Mutex, OnceLock,
        atomic::{AtomicI32, Ordering},
    },
};

const GUARD_BYTE: libc::off_t = HEADER_SIZE as libc::off_t - 1;
// Offsets, lengths and buffers of a direct read must all be aligned to this.
const DIRECT_ALIGNMENT: u64 = 4096;
type Key = (libc::dev_t, libc::ino_t);
struct Entry {
    shared: Arc<Shared>,
    users: usize,
    descriptors: Vec<OwnedFd>,
}
impl Entry {
    /// An exclusive guard needs a writable descriptor, and an earlier opener may have been
    /// able only to read. Holding the mutex means no guard is held through the old one.
    fn retain(&mut self, owned: OwnedFd) {
        if !writable(self.shared.fd.load(Ordering::Relaxed)) && writable(owned.as_raw_fd()) {
            let _idle = self.shared.mutex.lock().unwrap_or_else(|e| e.into_inner());
            self.shared.fd.store(owned.as_raw_fd(), Ordering::Relaxed);
        }
        self.descriptors.push(owned);
    }
}
struct Shared {
    // Changes only under the registry lock and `mutex`.
    fd: AtomicI32,
    mutex: Mutex<()>,
}
#[derive(Default)]
struct Registry {
    entries: HashMap<Key, Entry>,
    unclassified: Vec<OwnedFd>,
}
static FILES: OnceLock<Mutex<Registry>> = OnceLock::new();
fn files() -> &'static Mutex<Registry> {
    FILES.get_or_init(Default::default)
}
fn writable(fd: i32) -> bool {
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    flags & libc::O_ACCMODE != libc::O_RDONLY
}
fn errno() -> i32 {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}
fn failure(code: i32) -> Error {
    Error::new(code, std::io::Error::last_os_error().to_string())
}

/// A lease survives until *after* its delegated SQLite handle has closed.
/// Additional descriptors are retained: closing one early could drop POSIX locks.
pub struct Lease {
    key: Key,
    shared: Arc<Shared>,
}
#[derive(Clone)]
pub struct Access {
    shared: Arc<Shared>,
}
impl Lease {
    /// `denied` reports a file this directory refuses to hold, as native SQLite does.
    pub fn open(
        path: &CStr,
        create: bool,
        mode: libc::mode_t,
        denied: i32,
    ) -> Result<(Self, bool)> {
        let mut guard = files()
            .lock()
            .map_err(|_| Error::new(IOERR, "guard registry poisoned"))?;
        let registry = &mut *guard;
        let mut created = false;
        let mut refused = false;
        let mut fd = -1;
        // Reusing a known inode opens nothing, so nothing is left unclosable. The exception
        // is a file that may have become writable, and that adds at most one descriptor.
        let mut known: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::stat(path.as_ptr(), &mut known) } == 0 {
            let key = (known.st_dev, known.st_ino);
            if let Some(entry) = registry.entries.get_mut(&key) {
                if !writable(entry.shared.fd.load(Ordering::Relaxed)) {
                    fd = unsafe { libc::open(path.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
                }
                if fd >= 0 {
                    let owned = unsafe { OwnedFd::from_raw_fd(fd) };
                    let mut stat: libc::stat = unsafe { std::mem::zeroed() };
                    let same = unsafe { libc::fstat(fd, &mut stat) } == 0
                        && (stat.st_dev, stat.st_ino) == key;
                    if same {
                        entry.retain(owned);
                    } else {
                        registry.unclassified.push(owned);
                    }
                }
                entry.users += 1;
                let shared = entry.shared.clone();
                return Ok((Self { key, shared }, false));
            }
        }
        if create {
            fd = unsafe {
                libc::open(
                    path.as_ptr(),
                    libc::O_RDWR | libc::O_CLOEXEC | libc::O_CREAT | libc::O_EXCL,
                    libc::c_uint::from(mode),
                )
            };
            if fd >= 0 {
                created = true;
            } else {
                let error = errno();
                if !matches!(
                    error,
                    libc::EEXIST | libc::EACCES | libc::EROFS | libc::EPERM
                ) {
                    return Err(failure(CANTOPEN));
                }
                refused = error == libc::EACCES;
            }
        }
        if fd < 0 {
            fd = unsafe { libc::open(path.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
        }
        if fd < 0 && matches!(errno(), libc::EACCES | libc::EROFS | libc::EPERM) {
            // This process can only ever hold the guard shared, which a reader needs.
            fd = unsafe { libc::open(path.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC) };
        }
        if fd < 0 {
            return Err(failure(if refused { denied } else { CANTOPEN }));
        }
        let owned = unsafe { OwnedFd::from_raw_fd(fd) };
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(fd, &mut stat) } != 0 {
            let error = failure(IOERR_FSTAT);
            // Without its inode identity, closing this fd could release another
            // live SQLite handle's process-owned lock. Retain it until quiescent.
            if !registry.entries.is_empty() {
                registry.unclassified.push(owned);
            }
            return Err(error);
        }
        let key = (stat.st_dev, stat.st_ino);
        let entry = registry.entries.entry(key).or_insert_with(|| Entry {
            shared: Arc::new(Shared {
                fd: AtomicI32::new(owned.as_raw_fd()),
                mutex: Mutex::new(()),
            }),
            users: 0,
            descriptors: Vec::new(),
        });
        entry.retain(owned);
        entry.users += 1;
        Ok((
            Self {
                key,
                shared: entry.shared.clone(),
            },
            created,
        ))
    }

    /// Best-effort cleanup of a newly created temporary path after failed open.
    /// Do not follow a symlink or remove a replacement at the original name.
    pub fn unlink_if_same(&self, path: &CStr) {
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::lstat(path.as_ptr(), &mut stat) } == 0
            && (stat.st_dev, stat.st_ino) == self.key
        {
            unsafe { libc::unlink(path.as_ptr()) };
        }
    }

    pub fn access(&self) -> Access {
        Access {
            shared: self.shared.clone(),
        }
    }

    /// Reads past the page cache, whose unit can be larger than the sector that failed.
    /// The guard descriptor serves this because its use needs no new descriptor, and so
    /// no close that could drop POSIX locks. The caller holds the guard, so nothing else
    /// is using it. False means the bytes are unavailable, or the filesystem refused.
    pub fn read_direct(&self, offset: u64, bytes: &mut [u8]) -> bool {
        let fd = self.shared.fd.load(Ordering::Relaxed);
        if !set_direct(fd, true) {
            return false;
        }
        let done = read_aligned(fd, offset, bytes);
        set_direct(fd, false);
        done
    }
}
#[cfg(target_os = "macos")]
fn set_direct(fd: i32, direct: bool) -> bool {
    unsafe { libc::fcntl(fd, libc::F_NOCACHE, direct as i32) == 0 }
}
#[cfg(not(target_os = "macos"))]
fn set_direct(fd: i32, direct: bool) -> bool {
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    let flags = if direct {
        flags | libc::O_DIRECT
    } else {
        flags & !libc::O_DIRECT
    };
    unsafe { libc::fcntl(fd, libc::F_SETFL, flags) == 0 }
}
fn read_aligned(fd: i32, offset: u64, bytes: &mut [u8]) -> bool {
    let start = offset - offset % DIRECT_ALIGNMENT;
    let skip = (offset - start) as usize;
    let length = (skip + bytes.len()).next_multiple_of(DIRECT_ALIGNMENT as usize);
    let Ok(mut storage) = zeros::<u8>(length + DIRECT_ALIGNMENT as usize) else {
        return false;
    };
    let at = storage.as_ptr().align_offset(DIRECT_ALIGNMENT as usize);
    let aligned = &mut storage[at..at + length];
    let mut done = 0;
    while done < length {
        let got = unsafe {
            libc::pread(
                fd,
                aligned[done..].as_mut_ptr().cast(),
                length - done,
                (start + done as u64) as libc::off_t,
            )
        };
        if got < 0 && errno() == libc::EINTR {
            continue;
        }
        if got <= 0 {
            break;
        }
        done += got as usize;
    }
    if done < skip + bytes.len() {
        return false;
    }
    bytes.copy_from_slice(&aligned[skip..skip + bytes.len()]);
    true
}
impl Access {
    pub fn with<T>(&self, exclusive: bool, operation: impl FnOnce() -> Result<T>) -> Result<T> {
        let _local = self
            .shared
            .mutex
            .lock()
            .map_err(|_| Error::new(IOERR, "file guard poisoned"))?;
        let fd = self.shared.fd.load(Ordering::Relaxed);
        set_lock(
            fd,
            if exclusive {
                libc::F_WRLCK
            } else {
                libc::F_RDLCK
            },
            true,
        )?;
        let lock = Held(fd);
        let result = operation();
        let released = lock.release();
        match result {
            Err(e) => Err(e),
            Ok(value) => released.map(|_| value),
        }
    }
}
impl Drop for Lease {
    fn drop(&mut self) {
        // Even a poisoned registry must keep descriptor closure serialized.
        let mut registry = files().lock().unwrap_or_else(|e| e.into_inner());
        if let Some(entry) = registry.entries.get_mut(&self.key) {
            entry.users -= 1;
            if entry.users == 0 {
                registry.entries.remove(&self.key);
                if registry.entries.is_empty() {
                    registry.unclassified.clear();
                }
            }
        }
    }
}
fn set_lock(fd: i32, kind: impl Into<i32>, wait: bool) -> Result<()> {
    let mut lock: libc::flock = unsafe { std::mem::zeroed() };
    lock.l_type = kind.into() as _;
    lock.l_whence = libc::SEEK_SET as _;
    lock.l_start = GUARD_BYTE;
    lock.l_len = 1;
    loop {
        if unsafe {
            libc::fcntl(
                fd,
                if wait {
                    libc::F_OFD_SETLKW
                } else {
                    libc::F_OFD_SETLK
                },
                &lock,
            )
        } == 0
        {
            return Ok(());
        }
        if std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
            return Err(failure(IOERR_LOCK));
        }
    }
}
struct Held(i32);
impl Held {
    fn release(mut self) -> Result<()> {
        let fd = self.0;
        self.0 = -1;
        set_lock(fd, libc::F_UNLCK, false)
    }
}
impl Drop for Held {
    fn drop(&mut self) {
        if self.0 >= 0 {
            let _ = set_lock(self.0, libc::F_UNLCK, false);
        }
    }
}

#[cfg(test)]
#[path = "../tests/unit/guard.rs"]
mod tests;
