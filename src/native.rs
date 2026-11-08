//! Native SQLite handles retain the original sqlite3_filename and lock behavior.
use crate::{
    error::{CANTOPEN, Error, IOERR_DIR_FSYNC, IOERR_FSTAT, MISUSE, NOMEM, Result, TOOBIG},
    ffi,
    guard::Lease,
};
use std::{
    alloc::{Layout, alloc_zeroed, dealloc},
    ffi::{CStr, CString},
    ptr::NonNull,
};

// SQLite's unix VFS masks each transfer to 17 bits, so that it never asks for 128 KiB in
// one call: that would read and write nothing. A slot of 64 KiB pages in two copies does.
const MAX_TRANSFER: usize = 65536;

pub trait Physical {
    fn read(&mut self, offset: u64, bytes: &mut [u8]) -> Result<()>;
    fn write(&mut self, offset: u64, bytes: &[u8]) -> Result<()>;
    fn size(&mut self) -> Result<u64>;
    fn truncate(&mut self, length: u64) -> Result<()>;
    fn sync(&mut self, flags: i32) -> Result<()>;
}

pub struct Native {
    ptr: NonNull<ffi::sqlite3_file>,
    layout: Layout,
    pub lease: Option<Lease>,
    directory: Option<CString>,
    // Native unix file handles can retain this pointer for temporary names.
    _name: Option<CString>,
    closed: bool,
}
// Calls on a handle are serialized by its outer Io mutex. SQLite must be thread-safe.
unsafe impl Send for Native {}
impl Native {
    pub unsafe fn open(
        vfs: *mut ffi::sqlite3_vfs,
        name: *const std::ffi::c_char,
        owned_name: Option<CString>,
        flags: i32,
        guarded: bool,
        mode: libc::mode_t,
    ) -> Result<(Self, i32)> {
        let path = if let Some(s) = &owned_name {
            s.as_ptr()
        } else {
            name
        };
        // Allocate before the guard can create a file, so allocation errors leave
        // no temporary pathname to clean up.
        let layout = Layout::from_size_align(unsafe { (*vfs).szOsFile } as usize, 16)
            .map_err(|_| Error::new(MISUSE, "native file allocation is illegal"))?;
        let ptr = NonNull::new(unsafe { alloc_zeroed(layout) }.cast::<ffi::sqlite3_file>())
            .ok_or_else(|| Error::new(NOMEM, "native file allocation failed"))?;
        let mut this = Self {
            ptr,
            layout,
            lease: None,
            directory: None,
            _name: owned_name,
            closed: false,
        };
        let created = if guarded {
            if path.is_null() {
                return Err(Error::new(CANTOPEN, "protected file requires a path"));
            }
            let path = unsafe { CStr::from_ptr(path) };
            let create = flags & ffi::SQLITE_OPEN_CREATE != 0;
            let denied = if flags & ffi::SQLITE_OPEN_WAL != 0 {
                ffi::SQLITE_READONLY_DIRECTORY
            } else {
                CANTOPEN
            };
            let (lease, created) = Lease::open(path, create, mode, denied)?;
            this.lease = Some(lease);
            created
        } else {
            false
        };
        let mut actual = 0;
        // The guard already performed the exclusive creation. Repeating O_EXCL
        // would fail our own new temporary file and trigger a read-only fallback.
        let delegate_flags = if guarded && created {
            (flags & !ffi::SQLITE_OPEN_EXCLUSIVE) | ffi::SQLITE_OPEN_NOFOLLOW
        } else {
            flags
        };
        let code = unsafe {
            ((*vfs).xOpen.unwrap())(vfs, path, this.ptr.as_ptr(), delegate_flags, &mut actual)
        };
        if code != 0 {
            // Unix normally unlinks temporary files during open. On failure it
            // may not have reached that step; only remove the file we created.
            if created && flags & ffi::SQLITE_OPEN_DELETEONCLOSE != 0 {
                this.lease
                    .as_ref()
                    .unwrap()
                    .unlink_if_same(unsafe { CStr::from_ptr(path) });
            }
            let _ = this.close();
            return Err(Error::new(code, "native open failed"));
        }
        // SQLite falls back to read-only on main and the WAL, and handles both.
        let durable = flags & (ffi::SQLITE_OPEN_MAIN_DB | ffi::SQLITE_OPEN_WAL) != 0;
        if guarded && !durable && actual & ffi::SQLITE_OPEN_READWRITE == 0 {
            let _ = this.close();
            return Err(Error::new(
                ffi::SQLITE_READONLY,
                "temporary files require read/write access",
            ));
        }
        if created {
            use std::os::unix::ffi::OsStrExt;
            let p = std::path::Path::new(std::ffi::OsStr::from_bytes(
                unsafe { CStr::from_ptr(path) }.to_bytes(),
            ));
            let parent = p
                .parent()
                .filter(|p| !p.as_os_str().is_empty())
                .unwrap_or(std::path::Path::new("."));
            this.directory = Some(
                CString::new(parent.as_os_str().as_bytes())
                    .map_err(|_| Error::new(CANTOPEN, "file path contains NUL"))?,
            );
        }
        Ok((this, actual))
    }
    pub fn pointer(&self) -> *mut ffi::sqlite3_file {
        self.ptr.as_ptr()
    }
    pub fn methods(&self) -> &ffi::sqlite3_io_methods {
        unsafe { &*self.ptr.as_ref().pMethods }
    }
    pub fn close(&mut self) -> Result<()> {
        if self.closed {
            return Ok(());
        }
        self.closed = true;
        let methods = unsafe { self.ptr.as_ref().pMethods };
        let code = if methods.is_null() {
            0
        } else {
            unsafe { ((*methods).xClose.unwrap())(self.ptr.as_ptr()) }
        };
        // Only now may the last guard descriptor close.
        self.lease.take();
        check(code, "native close failed")
    }
    pub fn control(&mut self, op: i32, arg: *mut std::ffi::c_void) -> i32 {
        unsafe { (self.methods().xFileControl.unwrap())(self.ptr.as_ptr(), op, arg) }
    }
    pub fn lock(&mut self, level: i32) -> i32 {
        unsafe { (self.methods().xLock.unwrap())(self.ptr.as_ptr(), level) }
    }
    pub fn unlock(&mut self, level: i32) -> i32 {
        unsafe { (self.methods().xUnlock.unwrap())(self.ptr.as_ptr(), level) }
    }
}
// Plain fsync leaves the drive cache unflushed on macOS, which is no barrier.
#[cfg(target_os = "macos")]
fn durable_flags(flags: i32) -> i32 {
    (flags & !0x0f) | ffi::SQLITE_SYNC_FULL
}
#[cfg(not(target_os = "macos"))]
fn durable_flags(flags: i32) -> i32 {
    flags
}
fn sync_directory(fd: i32) -> i32 {
    #[cfg(target_os = "macos")]
    if unsafe { libc::fcntl(fd, libc::F_FULLFSYNC) } == 0 {
        return 0;
    }
    unsafe { libc::fsync(fd) }
}
fn check(code: i32, message: &str) -> Result<()> {
    if code == 0 {
        Ok(())
    } else {
        Err(Error::new(code, message))
    }
}
impl Native {
    fn read_part(&mut self, offset: u64, bytes: &mut [u8]) -> Result<()> {
        let n = i32::try_from(bytes.len()).map_err(|_| Error::new(TOOBIG, "read too large"))?;
        let off = i64::try_from(offset).map_err(|_| Error::new(TOOBIG, "read offset too large"))?;
        let result = check(
            unsafe {
                (self.methods().xRead.unwrap())(
                    self.ptr.as_ptr(),
                    bytes.as_mut_ptr().cast(),
                    n,
                    off,
                )
            },
            "native read failed",
        );
        // One bad sector fails a whole page-cache folio, which on Linux can hold the good
        // sectors beside it. Only the device can say which bytes are really gone.
        match (&result, &self.lease) {
            (Err(error), Some(lease)) if error.unreadable() && lease.read_direct(offset, bytes) => {
                Ok(())
            }
            _ => result,
        }
    }
    fn write_part(&mut self, offset: u64, bytes: &[u8]) -> Result<()> {
        let n = i32::try_from(bytes.len()).map_err(|_| Error::new(TOOBIG, "write too large"))?;
        let off =
            i64::try_from(offset).map_err(|_| Error::new(TOOBIG, "write offset too large"))?;
        check(
            unsafe {
                (self.methods().xWrite.unwrap())(self.ptr.as_ptr(), bytes.as_ptr().cast(), n, off)
            },
            "native write failed",
        )
    }
}
impl Physical for Native {
    fn read(&mut self, offset: u64, bytes: &mut [u8]) -> Result<()> {
        for (index, part) in bytes.chunks_mut(MAX_TRANSFER).enumerate() {
            self.read_part(offset + (index * MAX_TRANSFER) as u64, part)?;
        }
        Ok(())
    }
    fn write(&mut self, offset: u64, bytes: &[u8]) -> Result<()> {
        for (index, part) in bytes.chunks(MAX_TRANSFER).enumerate() {
            self.write_part(offset + (index * MAX_TRANSFER) as u64, part)?;
        }
        Ok(())
    }
    fn size(&mut self) -> Result<u64> {
        let mut n = 0;
        check(
            unsafe { (self.methods().xFileSize.unwrap())(self.ptr.as_ptr(), &mut n) },
            "native size failed",
        )?;
        u64::try_from(n).map_err(|_| Error::new(IOERR_FSTAT, "negative native size"))
    }
    fn truncate(&mut self, n: u64) -> Result<()> {
        check(
            unsafe {
                (self.methods().xTruncate.unwrap())(
                    self.ptr.as_ptr(),
                    i64::try_from(n).map_err(|_| Error::new(TOOBIG, "truncate too large"))?,
                )
            },
            "native truncate failed",
        )
    }
    fn sync(&mut self, flags: i32) -> Result<()> {
        check(
            unsafe { (self.methods().xSync.unwrap())(self.ptr.as_ptr(), durable_flags(flags)) },
            "native sync failed",
        )?;
        if let Some(path) = &self.directory {
            let fd = unsafe {
                libc::open(
                    path.as_ptr(),
                    libc::O_RDONLY | libc::O_CLOEXEC | libc::O_DIRECTORY,
                )
            };
            if fd < 0 {
                return Err(Error::new(IOERR_DIR_FSYNC, "directory open failed"));
            }
            let code = sync_directory(fd);
            let saved = std::io::Error::last_os_error();
            unsafe { libc::close(fd) };
            if code != 0 {
                return Err(Error::new(IOERR_DIR_FSYNC, saved.to_string()));
            }
            self.directory = None;
        }
        Ok(())
    }
}
impl Drop for Native {
    fn drop(&mut self) {
        let _ = self.close();
        unsafe { dealloc(self.ptr.as_ptr().cast(), self.layout) };
    }
}
