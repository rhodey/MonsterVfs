//! Directly mapped SQLite storage with checksums and bounded correction.

#[cfg(all(feature = "loadable", feature = "static"))]
compile_error!("loadable and static are mutually exclusive");
#[cfg(not(any(feature = "loadable", feature = "static")))]
compile_error!("select the loadable or static feature");
#[cfg(all(feature = "parity", feature = "copy"))]
compile_error!("parity and copy are mutually exclusive");
#[cfg(not(any(feature = "parity", feature = "copy")))]
compile_error!("select the parity or copy feature");

mod codec;
mod config;
#[cfg(feature = "copy")]
mod copy;
mod error;
#[cfg(feature = "parity")]
mod fec;
mod ffi;
mod file;
mod format;
mod guard;
mod maintenance;
mod native;
#[cfg(feature = "parity")]
mod parity;
mod recovery;
mod vfs;

#[cfg(test)]
#[path = "../tests/support/io_model.rs"]
mod test_io;

use std::ffi::{c_char, c_int};
use std::panic::{AssertUnwindSafe, catch_unwind};

unsafe fn initialize(
    api: *const ffi::sqlite3_api_routines,
    error: *mut *mut c_char,
) -> Result<(), c_int> {
    let api = unsafe { ffi::Api::capture(api, error) }?;
    let native = unsafe { api.vfs_find(c"unix".as_ptr()) };
    if native.is_null() {
        return Err(ffi::SQLITE_NOTFOUND);
    }
    vfs::register(api, native)
}

/// Register the VFS with the SQLite linked into the host application.
#[cfg(feature = "static")]
pub fn register() -> Result<(), i32> {
    unsafe { initialize(std::ptr::null(), std::ptr::null_mut()) }
}

/// Optionally stop maintenance permanently and join its worker.
/// Final SQLite connection teardown already waits for active maintenance.
pub fn shutdown() -> Result<(), i32> {
    let code = vfs::shutdown();
    if code == ffi::SQLITE_OK {
        Ok(())
    } else {
        Err(code)
    }
}

/// SQLite's run-time extension entry point.
///
/// # Safety
/// Called by SQLite with its valid host API table and error-output pointer.
#[cfg(feature = "loadable")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sqlite3_extension_init(
    _db: *mut ffi::sqlite3,
    error: *mut *mut c_char,
    api: *const ffi::sqlite3_api_routines,
) -> c_int {
    match catch_unwind(AssertUnwindSafe(|| unsafe { initialize(api, error) })) {
        Ok(Ok(())) => ffi::SQLITE_OK_LOAD_PERMANENTLY,
        Ok(Err(code)) => code,
        Err(_) => ffi::SQLITE_ERROR,
    }
}

#[cfg(feature = "static")]
#[unsafe(no_mangle)]
pub extern "C" fn monstervfs_register() -> c_int {
    match catch_unwind(AssertUnwindSafe(register)) {
        Ok(Ok(())) => ffi::SQLITE_OK,
        Ok(Err(code)) => code,
        Err(_) => ffi::SQLITE_ERROR,
    }
}

#[cfg(feature = "static")]
#[unsafe(no_mangle)]
pub extern "C" fn monstervfs_extra_init(_unused: *const c_char) -> c_int {
    monstervfs_register()
}

#[unsafe(no_mangle)]
pub extern "C" fn monstervfs_shutdown() -> c_int {
    match catch_unwind(AssertUnwindSafe(shutdown)) {
        Ok(Ok(())) => ffi::SQLITE_OK,
        Ok(Err(code)) => code,
        Err(_) => ffi::SQLITE_ERROR,
    }
}
