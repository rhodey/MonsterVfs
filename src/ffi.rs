//! Host SQLite ABI and immutable dispatch. Generated bindings live separately.
use std::ffi::{c_char, c_int, c_void};
use std::ptr::NonNull;
use std::sync::Arc;

#[allow(
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    dead_code
)]
mod bindings {
    include!("ffi_bindings.rs");
}
pub use bindings::*;

pub type AutoExtension =
    unsafe extern "C" fn(*mut sqlite3, *mut *mut c_char, *const sqlite3_api_routines) -> c_int;

pub struct Api {
    raw: NonNull<MvfsApi>,
}
// Capture verifies a thread-capable host. The dispatch object is immutable;
// individual SQLite connections remain subject to SQLite's ownership rules.
unsafe impl Send for Api {}
unsafe impl Sync for Api {}
impl Api {
    pub unsafe fn capture(
        source: *const sqlite3_api_routines,
        error: *mut *mut c_char,
    ) -> Result<Arc<Self>, c_int> {
        let mut raw = std::ptr::null_mut();
        let code = unsafe { mvfs_api_capture(source, &mut raw, error) };
        if code != SQLITE_OK {
            return Err(code);
        }
        let raw = NonNull::new(raw).ok_or(SQLITE_NOMEM)?;
        Ok(Arc::new(Self { raw }))
    }
    pub unsafe fn vfs_find(&self, name: *const c_char) -> *mut sqlite3_vfs {
        unsafe { mvfs_vfs_find(self.raw.as_ptr(), name) }
    }
    pub unsafe fn vfs_register(&self, vfs: *mut sqlite3_vfs, make_default: c_int) -> c_int {
        unsafe { mvfs_vfs_register(self.raw.as_ptr(), vfs, make_default) }
    }
    pub unsafe fn uri_parameter(
        &self,
        file: sqlite3_filename,
        name: *const c_char,
    ) -> *const c_char {
        unsafe { mvfs_uri_parameter(self.raw.as_ptr(), file, name) }
    }
    pub unsafe fn uri_boolean(
        &self,
        file: sqlite3_filename,
        name: *const c_char,
        fallback: c_int,
    ) -> c_int {
        unsafe { mvfs_uri_boolean(self.raw.as_ptr(), file, name, fallback) }
    }
    pub unsafe fn database_file_object(&self, file: sqlite3_filename) -> *mut sqlite3_file {
        unsafe { mvfs_database_file_object(self.raw.as_ptr(), file) }
    }
    pub unsafe fn open_v2(
        &self,
        name: *const c_char,
        db: *mut *mut sqlite3,
        flags: c_int,
        vfs: *const c_char,
    ) -> c_int {
        unsafe { mvfs_open_v2(self.raw.as_ptr(), name, db, flags, vfs) }
    }
    pub unsafe fn close(&self, db: *mut sqlite3) -> c_int {
        unsafe { mvfs_close(self.raw.as_ptr(), db) }
    }
    pub unsafe fn busy_timeout(&self, db: *mut sqlite3, ms: c_int) -> c_int {
        unsafe { mvfs_busy_timeout(self.raw.as_ptr(), db, ms) }
    }
    pub unsafe fn wal_autocheckpoint(&self, db: *mut sqlite3, pages: c_int) -> c_int {
        unsafe { mvfs_wal_autocheckpoint(self.raw.as_ptr(), db, pages) }
    }
    pub unsafe fn extended_errcode(&self, db: *mut sqlite3) -> c_int {
        unsafe { mvfs_extended_errcode(self.raw.as_ptr(), db) }
    }
    pub unsafe fn errmsg(&self, db: *mut sqlite3) -> *const c_char {
        unsafe { mvfs_errmsg(self.raw.as_ptr(), db) }
    }
    pub unsafe fn changes(&self, db: *mut sqlite3) -> c_int {
        unsafe { mvfs_changes(self.raw.as_ptr(), db) }
    }
    pub unsafe fn get_autocommit(&self, db: *mut sqlite3) -> c_int {
        unsafe { mvfs_get_autocommit(self.raw.as_ptr(), db) }
    }
    pub unsafe fn prepare_v2(
        &self,
        db: *mut sqlite3,
        sql: *const c_char,
        bytes: c_int,
        stmt: *mut *mut sqlite3_stmt,
        tail: *mut *const c_char,
    ) -> c_int {
        unsafe { mvfs_prepare_v2(self.raw.as_ptr(), db, sql, bytes, stmt, tail) }
    }
    pub unsafe fn bind_int64(&self, stmt: *mut sqlite3_stmt, index: c_int, value: i64) -> c_int {
        unsafe { mvfs_bind_int64(self.raw.as_ptr(), stmt, index, value) }
    }
    pub unsafe fn step(&self, stmt: *mut sqlite3_stmt) -> c_int {
        unsafe { mvfs_step(self.raw.as_ptr(), stmt) }
    }
    pub unsafe fn reset(&self, stmt: *mut sqlite3_stmt) -> c_int {
        unsafe { mvfs_reset(self.raw.as_ptr(), stmt) }
    }
    pub unsafe fn finalize(&self, stmt: *mut sqlite3_stmt) -> c_int {
        unsafe { mvfs_finalize(self.raw.as_ptr(), stmt) }
    }
    pub unsafe fn column_int64(&self, stmt: *mut sqlite3_stmt, column: c_int) -> i64 {
        unsafe { mvfs_column_int64(self.raw.as_ptr(), stmt, column) }
    }
    pub unsafe fn column_text(&self, stmt: *mut sqlite3_stmt, column: c_int) -> *const u8 {
        unsafe { mvfs_column_text(self.raw.as_ptr(), stmt, column) }
    }
    pub unsafe fn file_control(
        &self,
        db: *mut sqlite3,
        schema: *const c_char,
        op: c_int,
        arg: *mut c_void,
    ) -> c_int {
        unsafe { mvfs_file_control(self.raw.as_ptr(), db, schema, op, arg) }
    }
    pub unsafe fn db_readonly(&self, db: *mut sqlite3, schema: *const c_char) -> c_int {
        unsafe { mvfs_db_readonly(self.raw.as_ptr(), db, schema) }
    }
    pub unsafe fn connection_hook(
        &self,
        db: *mut sqlite3,
        name: *const c_char,
        data: *mut c_void,
        destroy: unsafe extern "C" fn(*mut c_void),
    ) -> c_int {
        unsafe { mvfs_connection_hook(self.raw.as_ptr(), db, name, data, Some(destroy)) }
    }
    pub unsafe fn auto_extension(&self, entry: AutoExtension) -> c_int {
        // SQLite declares the entry point with no arguments and calls it with these.
        let entry = unsafe { std::mem::transmute::<AutoExtension, unsafe extern "C" fn()>(entry) };
        unsafe { mvfs_auto_extension(self.raw.as_ptr(), Some(entry)) }
    }
    pub unsafe fn exec(
        &self,
        db: *mut sqlite3,
        sql: *const c_char,
        error: *mut *mut c_char,
    ) -> c_int {
        unsafe { mvfs_exec(self.raw.as_ptr(), db, sql, error) }
    }
    pub unsafe fn db_config_int(
        &self,
        db: *mut sqlite3,
        option: c_int,
        value: c_int,
        out: *mut c_int,
    ) -> c_int {
        unsafe { mvfs_db_config_int(self.raw.as_ptr(), db, option, value, out) }
    }
    pub unsafe fn log(&self, code: c_int, text: *const c_char) {
        unsafe { mvfs_log(self.raw.as_ptr(), code, text) }
    }
    pub unsafe fn strdup(&self, text: *const c_char) -> *mut c_char {
        unsafe { mvfs_strdup(self.raw.as_ptr(), text) }
    }
}
impl Drop for Api {
    fn drop(&mut self) {
        unsafe { mvfs_api_free(self.raw.as_ptr()) };
    }
}
