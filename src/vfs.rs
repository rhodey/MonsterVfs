//! SQLite callback adapter. Logical transactions remain entirely SQLite-owned.
use crate::{
    config::{ALIGNMENT, BLOCK_SIZE, Settings},
    error::{CORRUPT_BACKFILLED, Error, Result},
    ffi::{self, *},
    file::Mapped,
    format::{Kind, Profile, RESERVED_SIZE},
    maintenance::{self, Database, Service},
    native::{Native, Physical},
    recovery,
};
use std::{
    ffi::{CStr, CString, c_char, c_int, c_void},
    os::unix::ffi::OsStrExt,
    panic::{AssertUnwindSafe, catch_unwind},
    path::{Path, PathBuf},
    ptr,
    sync::{
        Arc, Mutex, OnceLock, Weak,
        atomic::{AtomicBool, Ordering},
    },
};

#[cfg(feature = "copy")]
const NAME: &CStr = c"monstervfs_copy";
#[cfg(feature = "parity")]
const NAME: &CStr = c"monstervfs_parity";

pub const FCNTL_ID: i32 = 0x4d560001;
pub const FCNTL_STATUS: i32 = 0x4d560002;
// Finds a main handle's state, including through a shim VFS stacked above us.
const FCNTL_STATE: i32 = 0x4d560003;
#[repr(C)]
struct StateRequest {
    owner: usize,
    state: *const State,
}
impl StateRequest {
    fn new() -> Self {
        Self {
            owner: reg().vfs,
            state: ptr::null(),
        }
    }
}

// SQLite's documented wal-index layout and SHM lock numbering.
const WAL_RECOVER_LOCK: i32 = 2;
const WAL_READ_LOCK_FIRST: i32 = 3;
const WAL_READ_LOCK_COUNT: i32 = 5;
const WAL_INDEX_REGION: i32 = 32768;
const WAL_BACKFILL_ATTEMPTED: usize = 128;

struct Registration {
    api: Arc<ffi::Api>,
    native: usize,
    vfs: usize,
    service: Arc<Service>,
}
static REG: OnceLock<Registration> = OnceLock::new();
static REGISTER: Mutex<()> = Mutex::new(());
fn reg() -> &'static Registration {
    REG.get().expect("registered VFS")
}
fn base() -> *mut sqlite3_vfs {
    reg().native as *mut _
}

pub fn register(api: Arc<ffi::Api>, native: *mut sqlite3_vfs) -> std::result::Result<(), i32> {
    let _lock = REGISTER.lock().map_err(|_| SQLITE_ERROR)?;
    let found = unsafe { api.vfs_find(NAME.as_ptr()) };
    if let Some(existing) = REG.get() {
        if !found.is_null() && found != existing.vfs as *mut _ {
            return Err(SQLITE_MISUSE);
        }
        // SQLite may have had its automatic extensions reset since the first load.
        register_reserve(&api)?;
        if found.is_null() {
            publish(&api, existing.vfs as *mut _);
        }
        return Ok(());
    }
    if !found.is_null() {
        return Err(SQLITE_MISUSE);
    }
    let mut vfs = Box::new(unsafe { ptr::read(native) });
    vfs.zName = NAME.as_ptr();
    vfs.pNext = ptr::null_mut();
    vfs.szOsFile = std::mem::size_of::<Wrapper>() as i32;
    vfs.xOpen = Some(open);
    vfs.xDelete = Some(delete);
    vfs.xAccess = Some(access);
    vfs.xFullPathname = Some(full_path);
    vfs.xDlOpen = Some(dl_open);
    vfs.xDlError = Some(dl_error);
    vfs.xDlSym = Some(dl_sym);
    vfs.xDlClose = Some(dl_close);
    vfs.xRandomness = Some(randomness);
    vfs.xSleep = Some(sleep);
    vfs.xCurrentTime = Some(current_time);
    vfs.xGetLastError = Some(last_error);
    vfs.xCurrentTimeInt64 = Some(current_time64);
    vfs.xSetSystemCall = Some(set_system_call);
    vfs.xGetSystemCall = Some(get_system_call);
    vfs.xNextSystemCall = Some(next_system_call);
    let service = Service::new(api.clone(), NAME.to_owned());
    // Keep both resources private until the last fallible host operation succeeds.
    // SQLite does not retain the callback on failure; join before it can unload us.
    if let Err(code) = register_reserve(&api) {
        let _ = service.shutdown();
        return Err(code);
    }
    // REGISTER excludes another initializer. No fallible work remains once SQLite
    // can invoke the automatic extension; cancellation cannot recall an in-flight call.
    let registration = REG.get_or_init(|| Registration {
        api: api.clone(),
        native: native as usize,
        vfs: Box::into_raw(vfs) as usize,
        service,
    });
    publish(&api, registration.vfs as *mut _);
    Ok(())
}
fn publish(api: &ffi::Api, vfs: *mut sqlite3_vfs) {
    // In the pinned SQLite, unix lookup already initialized the host, so registering
    // this non-null VFS only links it into the list and cannot fail. A violated host
    // contract must not return an error that unloads already published callbacks.
    if unsafe { api.vfs_register(vfs, 0) } != SQLITE_OK {
        std::process::abort();
    }
}
// SQL function destructors run after Btree teardown in the pinned SQLite. VFS
// xClose and TRACE_CLOSE run too early to wait for a private worker connection.
struct Connection {
    database: Arc<Database>,
    writable: bool,
    active: bool,
}
unsafe extern "C" fn destroy_connection(raw: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let connection = unsafe { Box::from_raw(raw.cast::<Connection>()) };
        if connection.active {
            let operation = connection.database.application_close(connection.writable);
            reg().service.wait_for_idle(operation);
        }
    }));
}
/// Set creation-time reserved bytes and track the logical connection lifetime.
unsafe extern "C" fn reserve_page_bytes(
    db: *mut sqlite3,
    _error: *mut *mut c_char,
    _api: *const sqlite3_api_routines,
) -> i32 {
    // A concurrent ordinary open may run this before our VFS is published.
    let Some(registration) = REG.get() else {
        return SQLITE_OK;
    };
    match catch_unwind(AssertUnwindSafe(|| {
        let api = &registration.api;
        let main = c"main".as_ptr();
        let mut request = StateRequest::new();
        let code = unsafe { api.file_control(db, main, FCNTL_STATE, (&raw mut request).cast()) };
        if code != SQLITE_OK || request.state.is_null() {
            return SQLITE_OK;
        }
        // The private control rejects other loaded codecs before exposing State.
        let state = unsafe { &*request.state };
        let mut reserved = RESERVED_SIZE as i32;
        unsafe {
            api.file_control(
                db,
                main,
                SQLITE_FCNTL_RESERVE_BYTES,
                (&raw mut reserved).cast(),
            )
        };
        if maintenance::is_worker() {
            return SQLITE_OK;
        }
        let Some(database) = &state.database else {
            return SQLITE_OK;
        };
        let writable = unsafe { api.db_readonly(db, main) } == 0;
        let connection = Box::into_raw(Box::new(Connection {
            database: database.clone(),
            writable,
            active: false,
        }));
        // Failed registration invokes xDestroy too, without activating this token.
        let code = unsafe {
            api.connection_hook(
                db,
                c"__monstervfs_connection".as_ptr(),
                connection.cast(),
                destroy_connection,
            )
        };
        if code == SQLITE_OK {
            database.application_open(writable);
            unsafe { (*connection).active = true };
        }
        code
    })) {
        Ok(code) => code,
        Err(_) => SQLITE_ERROR,
    }
}
/// Repeating this is harmless: SQLite keeps one entry for one function.
fn register_reserve(api: &ffi::Api) -> std::result::Result<(), i32> {
    let code = unsafe { api.auto_extension(reserve_page_bytes) };
    if code == SQLITE_OK { Ok(()) } else { Err(code) }
}
pub fn shutdown() -> i32 {
    match REG.get() {
        Some(r) => match r.service.shutdown() {
            Ok(()) => 0,
            Err(e) => e.code,
        },
        None => 0,
    }
}

#[repr(C)]
struct Wrapper {
    base: sqlite3_file,
    state: *mut State,
}
struct Context {
    wal: Mutex<Weak<Mutex<Io>>>,
    // SQLite then runs its private WAL recovery, which never reaches the SHM hook.
    shm_read_only: AtomicBool,
}
struct State {
    io: Arc<Mutex<Io>>,
    context: Option<Arc<Context>>,
    database: Option<Arc<Database>>,
    // Only writable handles may request persistent repairs.
    repairs: bool,
    prefix_token: Option<u64>,
}
struct Io {
    native: Native,
    mapped: Option<Mapped>,
    kind: Option<Kind>,
    // Nothing through this handle may change main or the WAL.
    read_only: bool,
    // The last place a read-only WAL handle stopped, so that it reports each once.
    limited: Option<u64>,
    failure: Option<Error>,
    closed: bool,
    shm_mapped: bool,
}
impl Io {
    fn active(&self) -> Result<()> {
        if self.closed {
            Err(Error::new(SQLITE_IOERR, "file is closed"))
        } else if let Some(e) = &self.failure {
            Err(e.clone())
        } else {
            Ok(())
        }
    }
    fn guarded<T>(
        &mut self,
        write: bool,
        operation: impl FnOnce(&mut Native, &mut Mapped) -> Result<T>,
    ) -> Result<T> {
        self.active()?;
        let access = self
            .native
            .lease
            .as_ref()
            .ok_or_else(|| Error::new(SQLITE_MISUSE, "file is not protected"))?
            .access();
        access.with(write, || {
            operation(&mut self.native, self.mapped.as_mut().unwrap())
        })
    }
    // Only an unknown file length or durability state stops a handle. A failed
    // write cannot: slots verify on every read, and a retry replaces them whole.
    fn poisoned<T>(&mut self, result: Result<T>) -> Result<T> {
        if let Err(error) = &result {
            self.failure.get_or_insert_with(|| error.clone());
        }
        result
    }
    /// Unfinished main files allow disposable inspection, not live adoption.
    /// An established main can still acquire a WAL initialized by another writer.
    fn adopt(&mut self) -> Result<bool> {
        let Some(lease) = &self.native.lease else {
            return Ok(false);
        };
        let access = lease.access();
        let native = &mut self.native;
        let Some(kind) = self.kind else {
            return Ok(false);
        };
        let mapped = access.with(false, || {
            if Mapped::unfinished(native, kind)? {
                return Ok(None);
            }
            if kind == Kind::Main {
                return Err(Error::new(
                    SQLITE_CANTOPEN,
                    "database initialized; reopen read-only connection",
                ));
            }
            Mapped::open(native).map(Some)
        })?;
        if let Some(m) = &mapped
            && m.profile.kind != kind
        {
            return Err(Error::corrupt("file kind differs"));
        }
        self.mapped = mapped;
        Ok(self.mapped.is_some())
    }
    fn unformatted(&self) -> bool {
        self.mapped.is_none() && self.native.lease.is_some()
    }
    fn flush_pending(&mut self) -> Result<()> {
        if self.mapped.as_ref().is_some_and(|m| m.pending.is_some()) {
            let result = self.guarded(true, |native, mapped| mapped.flush(native));
            if result.is_err() {
                self.mapped.as_mut().unwrap().pending = None;
            }
            result?;
        }
        Ok(())
    }
    fn lock_level(&mut self) -> Result<i32> {
        let mut value = 0;
        check(
            self.native
                .control(SQLITE_FCNTL_LOCKSTATE, (&mut value as *mut i32).cast()),
            "lock state failed",
        )?;
        Ok(value)
    }
    /// The last frame a running checkpoint may copy, from the shared wal-index.
    /// `None` means this connection has no shared index, never that reading one failed.
    fn backfill_limit(&self) -> Result<Option<u32>> {
        if !self.shm_mapped {
            return Ok(None);
        }
        let unavailable = || Error::new(SQLITE_IOERR_SHMMAP, "wal-index is unavailable");
        let map = self.native.methods().xShmMap.ok_or_else(unavailable)?;
        let mut region = ptr::null_mut();
        check(
            unsafe { map(self.native.pointer(), 0, WAL_INDEX_REGION, 0, &mut region) },
            "wal-index mapping failed",
        )?;
        if region.is_null() {
            return Err(unavailable());
        }
        Ok(Some(unsafe {
            ptr::read_volatile(
                region
                    .cast::<u8>()
                    .add(WAL_BACKFILL_ATTEMPTED)
                    .cast::<u32>(),
            )
        }))
    }
}
fn check(code: i32, message: &str) -> Result<()> {
    if code == 0 {
        Ok(())
    } else {
        Err(Error::new(code, message))
    }
}
unsafe fn state<'a>(file: *mut sqlite3_file) -> &'a State {
    unsafe { &*(*(file.cast::<Wrapper>())).state }
}
fn invoke(file: *mut sqlite3_file, task: impl FnOnce(&State) -> Result<()>) -> i32 {
    if file.is_null() {
        return SQLITE_MISUSE;
    }
    let result = catch_unwind(AssertUnwindSafe(|| task(unsafe { state(file) })));
    match result {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => {
            // Only the code crosses this boundary, so keep the reason for operators.
            if e.unreadable() {
                log(e.code, &format!("monstervfs: {}", e.message));
            }
            e.code
        }
        Err(_) => {
            let s = unsafe { state(file) };
            let mut io = s.io.lock().unwrap_or_else(|e| e.into_inner());
            io.failure = Some(Error::new(SQLITE_IOERR, "VFS callback panicked"));
            SQLITE_IOERR
        }
    }
}
fn lock(io: &Arc<Mutex<Io>>) -> Result<std::sync::MutexGuard<'_, Io>> {
    io.lock()
        .map_err(|_| Error::new(SQLITE_IOERR, "file state poisoned"))
}

unsafe extern "C" fn open(
    _vfs: *mut sqlite3_vfs,
    name: *const c_char,
    file: *mut sqlite3_file,
    flags: c_int,
    out: *mut c_int,
) -> c_int {
    if file.is_null() {
        return SQLITE_MISUSE;
    }
    unsafe {
        (*file).pMethods = ptr::null();
    }
    match catch_unwind(AssertUnwindSafe(|| open_inner(name, flags))) {
        Ok(Ok((s, actual))) => {
            unsafe {
                if !out.is_null() {
                    *out = actual;
                }
                let w = file.cast::<Wrapper>();
                (*w).state = Box::into_raw(Box::new(s));
                (*w).base.pMethods = &METHODS;
            }
            SQLITE_OK
        }
        Ok(Err(e)) => {
            if e.unreadable() {
                log(e.code, &format!("monstervfs: {}", e.message));
            }
            e.code
        }
        Err(_) => SQLITE_IOERR,
    }
}
fn open_inner(name: *const c_char, flags: i32) -> Result<(State, i32)> {
    let kind = if flags & SQLITE_OPEN_MAIN_DB != 0 {
        Some(Kind::Main)
    } else if flags & SQLITE_OPEN_WAL != 0 {
        Some(Kind::Wal)
    } else if flags
        & (SQLITE_OPEN_TEMP_DB
            | SQLITE_OPEN_TEMP_JOURNAL
            | SQLITE_OPEN_TRANSIENT_DB
            | SQLITE_OPEN_SUBJOURNAL)
        != 0
    {
        Some(Kind::Temporary)
    } else if flags & SQLITE_OPEN_MAIN_JOURNAL != 0 {
        None
    } else {
        return Err(Error::new(SQLITE_CANTOPEN, "unsupported SQLite file kind"));
    };
    let immutable = kind == Some(Kind::Main)
        && !name.is_null()
        && unsafe { reg().api.uri_boolean(name, c"immutable".as_ptr(), 0) } != 0;
    // SQLite refuses a WAL database on these too, as soon as it reads one.
    if kind == Some(Kind::Main)
        && !name.is_null()
        && unsafe { reg().api.uri_boolean(name, c"nolock".as_ptr(), 0) } != 0
    {
        return Err(Error::new(SQLITE_CANTOPEN, "no-lock access is unsupported"));
    }
    let mut owner = None;
    if kind == Some(Kind::Wal) {
        owner = Some(main_state(unsafe { reg().api.database_file_object(name) })?);
    }
    let settings = if let Some(main) = owner {
        let io = lock(&main.io)?;
        let missing = || Error::new(SQLITE_CANTOPEN, "database is not initialized");
        io.mapped.as_ref().ok_or_else(missing)?.profile.settings
    } else {
        Settings::default()
    };
    let requested = if kind == Some(Kind::Main) {
        uri_settings(name)?
    } else {
        None
    };
    let owned_name = if name.is_null() {
        let mut random = [0u8; 16];
        getrandom::fill(&mut random).map_err(|e| Error::new(SQLITE_CANTOPEN, e.to_string()))?;
        let suffix = random
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<String>();
        let directory = std::env::var_os("SQLITE_TMPDIR")
            .map(PathBuf::from)
            .unwrap_or_else(std::env::temp_dir);
        Some(
            CString::new(
                directory
                    .join(format!("monstervfs-{suffix}"))
                    .as_os_str()
                    .as_bytes(),
            )
            .map_err(|_| Error::new(SQLITE_CANTOPEN, "temporary path contains NUL"))?,
        )
    } else {
        None
    };
    let mode = creation_mode(kind, owner);
    let (mut native, actual) =
        unsafe { Native::open(base(), name, owned_name, flags, kind.is_some(), mode) }?;
    let read_only = actual & SQLITE_OPEN_READWRITE == 0
        || immutable
        || owner.is_some_and(|main| main.io.lock().is_ok_and(|io| io.read_only));
    let repairs = !read_only;
    let mapped = if let Some(kind) = kind {
        let guard = native.lease.as_ref().unwrap().access();
        guard.with(!read_only, || {
            // Holding the exclusive guard, no creation is running. So a file with
            // nothing durable in it is left over, and this opener may initialize it.
            let m = if Mapped::unfinished(&mut native, kind)? {
                if read_only {
                    return Ok(None);
                }
                if native.size()? != 0 {
                    native.truncate(0)?;
                }
                let mut id = [0; 16];
                getrandom::fill(&mut id).map_err(|e| Error::new(SQLITE_IOERR, e.to_string()))?;
                let profile = Profile::new(requested.unwrap_or(settings), kind, id)?;
                validate_alignment(&native)?;
                Mapped::create(&mut native, profile)?
            } else {
                Mapped::open(&mut native)?
            };
            validate_alignment(&native)?;
            if m.profile.kind != kind {
                return Err(Error::corrupt("file kind differs"));
            }
            if let Some(s) = requested
                && s != m.profile.settings
            {
                return Err(Error::new(SQLITE_CANTOPEN, "file settings differ"));
            }
            if owner.is_some() && m.profile.settings != settings {
                return Err(Error::corrupt("WAL settings differ from main"));
            }
            Ok(Some(m))
        })?
    } else {
        None
    };
    let database = if kind == Some(Kind::Main) {
        mapped.as_ref().map(|m| {
            reg()
                .service
                .register_database(unsafe { CStr::from_ptr(name) }.to_owned(), m.profile.id)
        })
    } else {
        owner.and_then(|s| s.database.as_ref().cloned())
    };
    let context = if kind == Some(Kind::Main) {
        Some(Arc::new(Context {
            wal: Mutex::new(Weak::new()),
            shm_read_only: AtomicBool::new(false),
        }))
    } else {
        owner.and_then(|s| s.context.clone())
    };
    let io = Arc::new(Mutex::new(Io {
        native,
        mapped,
        kind,
        read_only,
        limited: None,
        failure: None,
        closed: false,
        shm_mapped: false,
    }));
    if let Some(main) = owner {
        let mut main_io = lock(&main.io)?;
        if main_io.lock_level()? == SQLITE_LOCK_EXCLUSIVE {
            recover_wal(&mut main_io, &io, false, main.database.as_ref())?;
            let mut wal = lock(&io)?;
            if !wal.read_only {
                wal.guarded(false, |native, mapped| {
                    mapped.last_commit = recovery::last_commit(mapped, native)?;
                    Ok(())
                })?;
            }
        }
        *context
            .as_ref()
            .unwrap()
            .wal
            .lock()
            .map_err(|_| Error::new(SQLITE_IOERR, "WAL context poisoned"))? = Arc::downgrade(&io);
    }
    // A read-only descriptor cannot repair the prefix; any writable handle will.
    let prefix_token = if let Some(db) = database.as_ref().filter(|_| !read_only) {
        let id = lock(&io)?.mapped.as_ref().unwrap().profile.id;
        let weak = Arc::downgrade(&io);
        Some(db.register_prefix(
            id,
            Arc::new(move || {
                if let Some(io) = weak.upgrade() {
                    let mut file = lock(&io)?;
                    if !file.closed && file.failure.is_none() {
                        file.guarded(true, |native, mapped| {
                            mapped.repair_prefix(native).map(|_| ())
                        })?;
                        return Ok(true);
                    }
                }
                Ok(false)
            }),
        ))
    } else {
        None
    };
    if let Some(db) = &database {
        let file = lock(&io)?;
        if let Some(mapped) = &file.mapped
            && mapped.prefix_repair.iter().any(|v| *v)
            && repairs
        {
            db.observe_prefix(mapped.profile.id);
        }
    }
    Ok((
        State {
            io,
            context,
            database,
            repairs,
            prefix_token,
        },
        actual,
    ))
}
/// Matches what native SQLite would create: a WAL inherits its database's mode.
fn creation_mode(kind: Option<Kind>, owner: Option<&State>) -> libc::mode_t {
    const DEFAULT: libc::mode_t = 0o644;
    match kind {
        Some(Kind::Main) => DEFAULT,
        Some(Kind::Wal) => owner
            .and_then(|main| main.database.as_ref())
            .and_then(|database| std::fs::metadata(path_of(database.path())).ok())
            .map(|metadata| {
                use std::os::unix::fs::PermissionsExt;
                metadata.permissions().mode() as libc::mode_t & 0o777
            })
            .unwrap_or(DEFAULT),
        _ => 0o600,
    }
}
fn path_of(path: &CStr) -> &Path {
    Path::new(std::ffi::OsStr::from_bytes(path.to_bytes()))
}
fn main_state<'a>(main: *mut sqlite3_file) -> Result<&'a State> {
    let unavailable = || Error::new(SQLITE_CANTOPEN, "WAL main handle is unavailable");
    if main.is_null() {
        return Err(unavailable());
    }
    let control = unsafe { (*main).pMethods.as_ref() }
        .and_then(|methods| methods.xFileControl)
        .ok_or_else(unavailable)?;
    let mut request = StateRequest::new();
    let code = unsafe { control(main, FCNTL_STATE, (&raw mut request).cast()) };
    if code != SQLITE_OK {
        return Err(unavailable());
    }
    unsafe { request.state.as_ref() }.ok_or_else(unavailable)
}
fn validate_alignment(native: &Native) -> Result<()> {
    let sector = unsafe { (native.methods().xSectorSize.unwrap())(native.pointer()) };
    if sector <= 0 || !(sector as u32).is_power_of_two() || sector as usize > ALIGNMENT {
        return Err(Error::new(
            SQLITE_CANTOPEN,
            "native write-damage boundary exceeds 4096-byte alignment",
        ));
    }
    Ok(())
}
fn uri_settings(name: *const c_char) -> Result<Option<Settings>> {
    if name.is_null() {
        return Ok(None);
    }
    for key in [c"mv_block", c"mv_align"] {
        if !unsafe { reg().api.uri_parameter(name, key.as_ptr()) }.is_null() {
            return Err(Error::new(
                SQLITE_CANTOPEN,
                "mv_block and mv_align are unsupported; page size and alignment are fixed at 4096",
            ));
        }
    }
    let raw = unsafe { reg().api.uri_parameter(name, c"mv_parity".as_ptr()) };
    if raw.is_null() {
        return Ok(None);
    }
    if cfg!(feature = "copy") {
        return Err(Error::new(SQLITE_CANTOPEN, "this build has no mv_parity"));
    }
    let parity = unsafe { CStr::from_ptr(raw) }
        .to_str()
        .ok()
        .and_then(|s| s.parse().ok())
        .ok_or_else(|| Error::new(SQLITE_CANTOPEN, "illegal mv_parity setting"))?;
    Ok(Some(Settings::new(parity)?))
}

unsafe extern "C" fn close(file: *mut sqlite3_file) -> i32 {
    if file.is_null() {
        return SQLITE_MISUSE;
    }
    let w = file.cast::<Wrapper>();
    let raw = unsafe { (*w).state };
    if raw.is_null() {
        return SQLITE_OK;
    }
    unsafe {
        (*w).state = ptr::null_mut();
        (*w).base.pMethods = ptr::null();
    }
    match catch_unwind(AssertUnwindSafe(|| {
        let s = unsafe { Box::from_raw(raw) };
        if let Some(db) = s.database.as_ref()
            && let Some(token) = s.prefix_token
        {
            db.unregister_prefix(token);
        }
        let mut io = lock(&s.io)?;
        let _ = io.flush_pending();
        io.closed = true;
        io.native.close()
    })) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => e.code,
        Err(_) => SQLITE_IOERR_CLOSE,
    }
}
unsafe extern "C" fn read(file: *mut sqlite3_file, out: *mut c_void, n: i32, offset: i64) -> i32 {
    invoke(file, |s| {
        if n < 0 || offset < 0 || out.is_null() {
            return Err(Error::new(SQLITE_MISUSE, "illegal read"));
        }
        let output = unsafe { std::slice::from_raw_parts_mut(out.cast(), n as usize) };
        let mut io = lock(&s.io)?;
        io.active()?;
        if io.unformatted() && !io.adopt()? {
            output.fill(0);
            return Err(Error::new(SQLITE_IOERR_SHORT_READ, "file has no header"));
        }
        if io.mapped.is_none() {
            return io.native.read(offset as u64, output);
        }
        io.flush_pending()?;
        let profile = io.mapped.as_ref().unwrap().profile;
        let result = io.guarded(false, |native, mapped| {
            mapped.read(native, offset as u64, output)
        });
        // SQLite reads these bytes before it can reach the WAL, and uses only the page
        // and reserved sizes from them. If page one is really lost, its full read fails.
        if let Err(error) = &result
            && error.unreadable()
            && profile.kind == Kind::Main
            && offset == 0
            && n <= 100
        {
            output.fill(0);
            if n >= 18 {
                let encoded = BLOCK_SIZE as u16;
                output[..16].copy_from_slice(b"SQLite format 3\0");
                output[16..18].copy_from_slice(&encoded.to_be_bytes());
            }
            if let Some(reserved) = output.get_mut(20) {
                *reserved = RESERVED_SIZE as u8;
            }
            return Ok(());
        }
        let outcome = result?;
        drop(io);
        if let Some(db) = s.database.as_ref() {
            for o in outcome.observations {
                if profile.kind == Kind::Main && s.repairs && !maintenance::is_worker() {
                    db.observe_main(o.block, o.fingerprint);
                } else if profile.kind == Kind::Wal {
                    db.observe_wal(profile.id, o.block);
                }
            }
            if profile.kind == Kind::Main {
                db.observe_zero_slots(outcome.zero_slots);
            }
        }
        if outcome.short {
            Err(Error::new(SQLITE_IOERR_SHORT_READ, "logical short read"))
        } else {
            Ok(())
        }
    })
}
unsafe extern "C" fn write(
    file: *mut sqlite3_file,
    input: *const c_void,
    n: i32,
    offset: i64,
) -> i32 {
    invoke(file, |s| {
        if n < 0 || offset < 0 || input.is_null() {
            return Err(Error::new(SQLITE_MISUSE, "illegal write"));
        }
        let input = unsafe { std::slice::from_raw_parts(input.cast(), n as usize) };
        let mut io = lock(&s.io)?;
        io.active()?;
        if io.read_only {
            return Err(Error::new(SQLITE_IOERR_WRITE, "file is read-only"));
        }
        if io.mapped.is_none() {
            return io.native.write(offset as u64, input);
        }
        let kind = io.mapped.as_ref().unwrap().profile.kind;
        let id = io.mapped.as_ref().unwrap().profile.id;
        let result = io.guarded(true, |native, mapped| {
            mapped.write(native, offset as u64, input)
        });
        // SQLite may release its writer lock after this error. Never replay the
        // abandoned header over a later writer's frame, including during close.
        if result.is_err() {
            io.mapped.as_mut().unwrap().pending = None;
        }
        let blocks = result?;
        drop(io);
        if let Some(db) = s.database.as_ref() {
            for block in blocks {
                if kind == Kind::Main {
                    db.retire_main(block);
                } else if kind == Kind::Wal {
                    db.retire_wal_block(id, block);
                }
            }
        }
        Ok(())
    })
}
unsafe extern "C" fn truncate(file: *mut sqlite3_file, length: i64) -> i32 {
    invoke(file, |s| {
        if length < 0 {
            return Err(Error::new(SQLITE_MISUSE, "negative truncate"));
        }
        let mut io = lock(&s.io)?;
        io.active()?;
        if io.mapped.is_none() {
            return io.native.truncate(length as u64);
        }
        io.flush_pending()?;
        let profile = io.mapped.as_ref().unwrap().profile;
        let result = io.guarded(true, |native, mapped| {
            mapped.truncate(native, length as u64)
        });
        io.poisoned(result)?;
        drop(io);
        if let Some(db) = s.database.as_ref() {
            let retained = profile
                .truncated_length(length as u64)
                .saturating_sub(profile.logical_base())
                .div_ceil(profile.payload_size() as u64);
            if profile.kind == Kind::Wal {
                db.retire_wal(profile.id, retained);
            } else if profile.kind == Kind::Main {
                db.retire_main_from(retained);
            }
        }
        Ok(())
    })
}
unsafe extern "C" fn sync(file: *mut sqlite3_file, flags: i32) -> i32 {
    invoke(file, |s| {
        let mut io = lock(&s.io)?;
        io.active()?;
        io.flush_pending()?;
        // Slot coherence needs no guard here. Holding it would stall every reader.
        let result = io.native.sync(flags);
        // In WAL mode, syncing main is the end of a checkpoint. If it fails nothing is left
        // unknown: the checkpoint did not finish, so the WAL stays in charge and the next
        // one rewrites every page. A commit's durability is unknown, so a WAL sync stops.
        if io.kind == Some(Kind::Main) {
            if let (Err(_), Some(db)) = (&result, s.database.as_ref()) {
                db.record_main_sync_failure();
            }
            result
        } else if io.mapped.is_some() {
            io.poisoned(result)
        } else {
            result
        }
    })
}
unsafe extern "C" fn size(file: *mut sqlite3_file, out: *mut i64) -> i32 {
    invoke(file, |s| {
        if out.is_null() {
            return Err(Error::new(SQLITE_MISUSE, "null size"));
        }
        let mut io = lock(&s.io)?;
        io.active()?;
        io.flush_pending()?;
        if io.unformatted() {
            io.adopt()?;
        }
        let limited =
            io.kind == Some(Kind::Wal) && io.mapped.is_some() && (io.read_only || shm_read_only(s));
        let limit = if limited {
            wal_limit(&mut io, s.database.as_ref())?
        } else {
            None
        };
        let n = if io.mapped.is_some() {
            io.guarded(false, |native, mapped| mapped.size(native))?
        } else if io.unformatted() && io.kind == Some(Kind::Wal) {
            0
        } else {
            io.native.size()?
        };
        let n = limit.map_or(n, |limit| n.min(limit));
        unsafe {
            *out = i64::try_from(n).map_err(|_| Error::new(SQLITE_TOOBIG, "file too large"))?;
        }
        Ok(())
    })
}
unsafe extern "C" fn file_lock(file: *mut sqlite3_file, level: i32) -> i32 {
    invoke(file, |s| {
        let mut io = lock(&s.io)?;
        io.active()?;
        check(io.native.lock(level), "native lock failed")
    })
}
unsafe extern "C" fn unlock(file: *mut sqlite3_file, level: i32) -> i32 {
    invoke(file, |s| {
        let mut io = lock(&s.io)?;
        check(io.native.unlock(level), "native unlock failed")
    })
}
unsafe extern "C" fn reserved(file: *mut sqlite3_file, out: *mut i32) -> i32 {
    invoke(file, |s| {
        let io = lock(&s.io)?;
        io.active()?;
        check(
            unsafe { (io.native.methods().xCheckReservedLock.unwrap())(io.native.pointer(), out) },
            "reserved-lock check failed",
        )
    })
}

unsafe extern "C" fn control(file: *mut sqlite3_file, op: i32, arg: *mut c_void) -> i32 {
    invoke(file, |s| {
        let mut io = lock(&s.io)?;
        if io.mapped.is_none() {
            return check(io.native.control(op, arg), "native control failed");
        }
        match op {
            FCNTL_STATE => {
                if arg.is_null() {
                    return Err(Error::new(SQLITE_MISUSE, "null state output"));
                }
                let request = unsafe { &mut *arg.cast::<StateRequest>() };
                if request.owner != reg().vfs {
                    return Err(Error::new(SQLITE_NOTFOUND, "different VFS owner"));
                }
                request.state = s;
                Ok(())
            }
            FCNTL_ID => {
                if arg.is_null() {
                    return Err(Error::new(SQLITE_MISUSE, "null identity output"));
                }
                unsafe {
                    ptr::copy_nonoverlapping(
                        io.mapped.as_ref().unwrap().profile.id.as_ptr(),
                        arg.cast(),
                        16,
                    );
                }
                Ok(())
            }
            FCNTL_STATUS => {
                drop(io);
                put_string(
                    arg,
                    s.database
                        .as_ref()
                        .map(|db| db.snapshot().json())
                        .unwrap_or_else(|| "{}".into()),
                )
            }
            SQLITE_FCNTL_FILE_POINTER => {
                if arg.is_null() {
                    return Err(Error::new(SQLITE_MISUSE, "null file output"));
                }
                unsafe {
                    *arg.cast::<*mut sqlite3_file>() = file;
                }
                Ok(())
            }
            SQLITE_FCNTL_VFS_POINTER => {
                if arg.is_null() {
                    return Err(Error::new(SQLITE_MISUSE, "null VFS output"));
                }
                unsafe {
                    *arg.cast::<*mut sqlite3_vfs>() = reg().vfs as *mut _;
                }
                Ok(())
            }
            SQLITE_FCNTL_VFSNAME => put_string(arg, format!("{}/unix", NAME.to_string_lossy())),
            SQLITE_FCNTL_MMAP_SIZE => {
                if !arg.is_null() {
                    unsafe {
                        *arg.cast::<i64>() = 0;
                    }
                }
                Ok(())
            }
            SQLITE_FCNTL_POWERSAFE_OVERWRITE => {
                if !arg.is_null() {
                    let kind = io.mapped.as_ref().unwrap().profile.kind;
                    unsafe {
                        *arg.cast::<i32>() = (kind != Kind::Temporary) as i32;
                    }
                }
                Ok(())
            }
            SQLITE_FCNTL_CKPT_START => {
                // Its main writes cannot land, so the watermark must not move.
                if io.read_only {
                    return Ok(());
                }
                let result = record_backfill(&io, s);
                // SQLite ignores this result, so the main writes must fail instead.
                io.poisoned(result)
            }
            SQLITE_FCNTL_PRAGMA => {
                if arg.is_null() {
                    return Err(Error::new(SQLITE_MISUSE, "null pragma"));
                }
                let args = arg.cast::<*mut c_char>();
                let name = unsafe { *args.add(1) };
                let value = unsafe { *args.add(2) };
                if name.is_null() {
                    return Err(Error::new(SQLITE_NOTFOUND, "no pragma name"));
                }
                let name = unsafe { CStr::from_ptr(name) }.to_bytes();
                if name.eq_ignore_ascii_case(b"monstervfs_maintenance") {
                    drop(io);
                    return put_string(
                        arg,
                        s.database
                            .as_ref()
                            .map(|db| db.snapshot().json())
                            .unwrap_or_else(|| "{}".into()),
                    );
                }
                if !value.is_null() && io.mapped.as_ref().unwrap().profile.kind == Kind::Main {
                    let value = unsafe { CStr::from_ptr(value) }.to_bytes();
                    // A reader cannot change these, and SQLite tells it so itself.
                    let illegal = if name.eq_ignore_ascii_case(b"journal_mode") {
                        !io.read_only && !value.eq_ignore_ascii_case(b"wal")
                    } else if name.eq_ignore_ascii_case(b"synchronous") {
                        !io.read_only && !value.eq_ignore_ascii_case(b"full") && value != b"2"
                    } else if name.eq_ignore_ascii_case(b"page_size") {
                        !io.read_only
                            && std::str::from_utf8(value)
                                .ok()
                                .and_then(|v| v.parse::<usize>().ok())
                                != Some(BLOCK_SIZE)
                    } else if name.eq_ignore_ascii_case(b"mmap_size") {
                        value != b"0"
                    } else {
                        false
                    };
                    // Upstream scripts only: absorb the setting, so that they reach what they test.
                    #[cfg(feature = "lenient-pragmas")]
                    if illegal && std::env::var_os("MONSTERVFS_LENIENT").is_some() {
                        if name.eq_ignore_ascii_case(b"journal_mode") {
                            put_string(arg, "wal".into())?;
                        }
                        return Ok(());
                    }
                    if illegal {
                        put_string(
                            arg,
                            "monstervfs requires WAL/FULL, 4096-byte pages, and mmap disabled"
                                .into(),
                        )?;
                        return Err(Error::new(SQLITE_ERROR, "unsupported pragma setting"));
                    }
                }
                Err(Error::new(SQLITE_NOTFOUND, "pragma belongs to SQLite"))
            }
            SQLITE_FCNTL_LOCKSTATE
            | SQLITE_FCNTL_LAST_ERRNO
            | SQLITE_FCNTL_PERSIST_WAL
            | SQLITE_FCNTL_HAS_MOVED
            | SQLITE_FCNTL_EXTERNAL_READER
            | SQLITE_FCNTL_TEMPFILENAME
            | SQLITE_FCNTL_LOCK_TIMEOUT => {
                check(io.native.control(op, arg), "native control failed")
            }
            // These only hint at growth, and the native file must not grow by them:
            // its length is what says how much of the logical file exists.
            SQLITE_FCNTL_CHUNK_SIZE | SQLITE_FCNTL_SIZE_HINT => Ok(()),
            SQLITE_FCNTL_SYNC | SQLITE_FCNTL_COMMIT_PHASETWO => Ok(()),
            _ => Err(Error::new(SQLITE_NOTFOUND, "unsupported file control")),
        }
    })
}
fn put_string(arg: *mut c_void, value: String) -> Result<()> {
    if arg.is_null() {
        return Err(Error::new(SQLITE_MISUSE, "null string output"));
    }
    let value = CString::new(value).map_err(|_| Error::new(SQLITE_ERROR, "status contains NUL"))?;
    let raw = unsafe { reg().api.strdup(value.as_ptr()) };
    if raw.is_null() {
        return Err(Error::new(SQLITE_NOMEM, "string allocation failed"));
    }
    unsafe {
        *arg.cast::<*mut c_char>() = raw;
    }
    Ok(())
}
unsafe extern "C" fn sector(file: *mut sqlite3_file) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let s = unsafe { state(file) };
        let io = lock(&s.io)?;
        Ok::<_, Error>(if io.mapped.is_some() {
            BLOCK_SIZE as i32
        } else {
            unsafe { (io.native.methods().xSectorSize.unwrap())(io.native.pointer()) }
        })
    }))
    .ok()
    .and_then(|x| x.ok())
    .unwrap_or(4096)
}
unsafe extern "C" fn characteristics(file: *mut sqlite3_file) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let s = unsafe { state(file) };
        let io = lock(&s.io)?;
        Ok::<_, Error>(if let Some(m) = &io.mapped {
            // Pages and frames each own a slot, so a write cannot damage a neighbor.
            if m.profile.kind == Kind::Temporary {
                0
            } else {
                SQLITE_IOCAP_POWERSAFE_OVERWRITE
            }
        } else {
            unsafe { (io.native.methods().xDeviceCharacteristics.unwrap())(io.native.pointer()) }
        })
    }))
    .ok()
    .and_then(|x| x.ok())
    .unwrap_or(0)
}
unsafe extern "C" fn shm_map(
    file: *mut sqlite3_file,
    page: i32,
    bytes: i32,
    extend: i32,
    out: *mut *mut c_void,
) -> i32 {
    invoke(file, |s| {
        let mut io = lock(&s.io)?;
        io.active()?;
        let method = io
            .native
            .methods()
            .xShmMap
            .ok_or_else(|| Error::new(SQLITE_IOERR_SHMMAP, "native SHM unavailable"))?;
        let code = unsafe { method(io.native.pointer(), page, bytes, extend, out) };
        if code & 255 == SQLITE_READONLY {
            if let Some(context) = &s.context {
                context.shm_read_only.store(true, Ordering::Relaxed);
            }
            io.shm_mapped = code == SQLITE_READONLY;
            return Err(Error::new(code, "shared memory is read-only"));
        }
        check(code, "native SHM mapping failed")?;
        io.shm_mapped = true;
        Ok(())
    })
}
// SQLite will not release a lock it was told it never received. A release that
// fails leaves an unknown lock behind, so that handle must stop.
fn unwind_recovery(
    failure: &mut Option<Error>,
    original: Error,
    release: impl FnOnce() -> Result<()>,
) -> Error {
    if let Err(error) = release() {
        failure.get_or_insert(error);
    }
    original
}
unsafe extern "C" fn shm_lock(file: *mut sqlite3_file, offset: i32, n: i32, flags: i32) -> i32 {
    invoke(file, |s| {
        let mut io = lock(&s.io)?;
        if flags & SQLITE_SHM_LOCK != 0 {
            io.active()?;
        }
        let method = io
            .native
            .methods()
            .xShmLock
            .ok_or_else(|| Error::new(SQLITE_IOERR_SHMLOCK, "native SHM unavailable"))?;
        check(
            unsafe { method(io.native.pointer(), offset, n, flags) },
            "native SHM lock failed",
        )?;
        let covers_recovery = offset <= WAL_RECOVER_LOCK
            && offset
                .checked_add(n)
                .is_some_and(|end| end > WAL_RECOVER_LOCK);
        if covers_recovery
            && flags & (SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE)
                == (SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE)
            && let Some(wal) = wal_of(s)?
            && let Err(error) = recover_wal(&mut io, &wal, true, s.database.as_ref())
        {
            // Closing runs recovery in exclusive mode, where SQLite takes no SHM locks and
            // so never reaches this hook. Unless this handle stays failed, that recovery
            // would stop at the frame this one refused, checkpoint, and delete the WAL.
            lock(&wal)?.failure.get_or_insert_with(|| error.clone());
            let native = io.native.pointer();
            let release = || {
                check(
                    unsafe { method(native, offset, n, SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE) },
                    "recovery SHM unwind failed",
                )
            };
            return Err(unwind_recovery(&mut io.failure, error, release));
        }
        Ok(())
    })
}
fn shm_read_only(state: &State) -> bool {
    state
        .context
        .as_ref()
        .is_some_and(|context| context.shm_read_only.load(Ordering::Relaxed))
}
fn wal_of(state: &State) -> Result<Option<Arc<Mutex<Io>>>> {
    let Some(context) = &state.context else {
        return Ok(None);
    };
    let wal = context
        .wal
        .lock()
        .map_err(|_| Error::new(SQLITE_IOERR, "WAL context poisoned"))?;
    Ok(wal.upgrade())
}
/// Durable before the first main write, so recovery knows what main may hold.
fn record_backfill(main: &Io, state: &State) -> Result<()> {
    let wal =
        wal_of(state)?.ok_or_else(|| Error::new(SQLITE_IOERR, "checkpoint has no WAL handle"))?;
    let limit = main.backfill_limit()?;
    let mut wal = lock(&wal)?;
    wal.flush_pending()?;
    wal.guarded(true, |native, mapped| {
        let frames = limit.unwrap_or(u32::try_from(mapped.last_commit).unwrap_or(u32::MAX));
        mapped.record_backfill(native, frames)
    })?;
    let result = wal.native.sync(SQLITE_SYNC_NORMAL);
    wal.poisoned(result)
}
fn scan_wal(wal: &mut Io, database: Option<&Arc<Database>>) -> Result<recovery::Scan> {
    let scan = wal.guarded(false, |native, mapped| recovery::scan(mapped, native));
    if let (Err(error), Some(db)) = (&scan, database)
        && error.code == CORRUPT_BACKFILLED
    {
        db.record_refusal();
        log(
            SQLITE_WARNING,
            &format!("monstervfs refused WAL recovery: {}", error.message),
        );
    }
    scan
}
/// The same decision as a recovery, for a connection that must not change the WAL.
/// SQLite stops at the first unusable frame of its own accord, and its recovery reads no
/// further than the size it was given, on the private path too. So reporting a shorter
/// WAL gives this connection what a cut would have left, and leaves the bytes alone.
fn wal_limit(wal: &mut Io, database: Option<&Arc<Database>>) -> Result<Option<u64>> {
    let scan = scan_wal(wal, database)?;
    let Some(cutoff) = scan.cutoff else {
        return Ok(None);
    };
    if wal.limited != Some(cutoff.offset) {
        wal.limited = Some(cutoff.offset);
        if let Some(db) = database {
            let physical = wal.guarded(false, |native, _| native.size())?;
            let hidden = physical.saturating_sub(cutoff.offset);
            db.record_cutoff(cutoff.offset, hidden, cutoff.lost_commits);
        }
        log(
            cutoff_code(&cutoff),
            &format!(
                "monstervfs read-only connection stopped at WAL byte {}: {} finished commits follow it",
                cutoff.offset, cutoff.lost_commits
            ),
        );
    }
    let profile = wal.mapped.as_ref().unwrap().profile;
    profile.logical_length(cutoff.offset).map(Some)
}
fn recover_wal(
    main: &mut Io,
    wal: &Arc<Mutex<Io>>,
    shared_memory: bool,
    database: Option<&Arc<Database>>,
) -> Result<()> {
    let mut wal = lock(wal)?;
    if wal.closed || wal.read_only {
        return Ok(());
    }
    let scan = scan_wal(&mut wal, database)?;
    let profile = wal.mapped.as_ref().unwrap().profile;
    if let Some(db) = database {
        for observation in &scan.observations {
            db.observe_wal(profile.id, observation.block);
        }
    }
    // Corrected contents are returned by normal reads; only cutoff mutates here.
    let Some(cutoff) = scan.cutoff else {
        return Ok(());
    };
    // Holding every WAL lock excludes active WAL users. Idle connections keep
    // their SHARED main lock throughout, so they must not be able to block this.
    let shm = main
        .native
        .methods()
        .xShmLock
        .filter(|_| shared_memory)
        .map(|method| (method, main.native.pointer()));
    if let Some((method, native)) = shm {
        let flags = SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE;
        check(
            unsafe { method(native, WAL_READ_LOCK_FIRST, WAL_READ_LOCK_COUNT, flags) },
            "active WAL readers block recovery",
        )?;
    }
    let applied = apply_cutoff(&mut wal, &cutoff, profile, database);
    if let Some((method, native)) = shm {
        let flags = SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE;
        let released = check(
            unsafe { method(native, WAL_READ_LOCK_FIRST, WAL_READ_LOCK_COUNT, flags) },
            "recovery read-lock release failed",
        );
        if let Err(error) = released {
            main.failure.get_or_insert(error);
        }
    }
    applied
}
/// An ordinary crash tears only the tail, which recovery handles without any alarm.
fn cutoff_code(cutoff: &recovery::Cutoff) -> i32 {
    if cutoff.lost_commits > 0 {
        SQLITE_WARNING
    } else {
        SQLITE_NOTICE_RECOVER_WAL
    }
}
fn apply_cutoff(
    wal: &mut Io,
    cutoff: &recovery::Cutoff,
    profile: Profile,
    database: Option<&Arc<Database>>,
) -> Result<()> {
    let physical = wal.guarded(false, |native, _| native.size())?;
    let result = wal.guarded(true, |native, _| {
        native.truncate(cutoff.offset)?;
        native.sync(SQLITE_SYNC_FULL)
    });
    wal.poisoned(result)?;
    let discarded = physical.saturating_sub(cutoff.offset);
    if let Some(db) = database {
        db.retire_wal(
            profile.id,
            (cutoff.offset - profile.content_start()) / profile.slot_size() as u64,
        );
        db.record_cutoff(cutoff.offset, discarded, cutoff.lost_commits);
    }
    log(
        cutoff_code(cutoff),
        &format!(
            "monstervfs cut WAL at byte {}: {discarded} bytes, {} frames and {} finished commits discarded",
            cutoff.offset, cutoff.later_frames, cutoff.lost_commits
        ),
    );
    Ok(())
}
fn log(code: i32, message: &str) {
    if let Ok(text) = CString::new(message) {
        unsafe { reg().api.log(code, text.as_ptr()) };
    }
}
unsafe extern "C" fn shm_barrier(file: *mut sqlite3_file) {
    let _ = invoke(file, |s| {
        let io = lock(&s.io)?;
        if let Some(method) = io.native.methods().xShmBarrier {
            unsafe { method(io.native.pointer()) };
        }
        Ok(())
    });
}
unsafe extern "C" fn shm_unmap(file: *mut sqlite3_file, delete: i32) -> i32 {
    invoke(file, |s| {
        let io = lock(&s.io)?;
        if let Some(method) = io.native.methods().xShmUnmap {
            check(
                unsafe { method(io.native.pointer(), delete) },
                "native SHM unmap failed",
            )
        } else {
            Ok(())
        }
    })
}
unsafe extern "C" fn fetch(
    _file: *mut sqlite3_file,
    _off: i64,
    _n: i32,
    out: *mut *mut c_void,
) -> i32 {
    if out.is_null() {
        return SQLITE_MISUSE;
    }
    unsafe {
        *out = ptr::null_mut();
    }
    SQLITE_OK
}
unsafe extern "C" fn unfetch(_file: *mut sqlite3_file, _off: i64, _p: *mut c_void) -> i32 {
    SQLITE_OK
}
static METHODS: sqlite3_io_methods = sqlite3_io_methods {
    iVersion: 3,
    xClose: Some(close),
    xRead: Some(read),
    xWrite: Some(write),
    xTruncate: Some(truncate),
    xSync: Some(sync),
    xFileSize: Some(size),
    xLock: Some(file_lock),
    xUnlock: Some(unlock),
    xCheckReservedLock: Some(reserved),
    xFileControl: Some(control),
    xSectorSize: Some(sector),
    xDeviceCharacteristics: Some(characteristics),
    xShmMap: Some(shm_map),
    xShmLock: Some(shm_lock),
    xShmBarrier: Some(shm_barrier),
    xShmUnmap: Some(shm_unmap),
    xFetch: Some(fetch),
    xUnfetch: Some(unfetch),
};

// Native VFS services are called with their original VFS pointer and app data.
macro_rules! delegate {($name:ident,$field:ident,($($arg:ident:$ty:ty),*)->$ret:ty)=>{unsafe extern "C" fn $name(_vfs:*mut sqlite3_vfs,$($arg:$ty),*)->$ret {unsafe{((*base()).$field.unwrap())(base(),$($arg),*)}}};}
delegate!(delete,xDelete,(name:*const c_char,sync_dir:i32)->i32);
delegate!(access,xAccess,(name:*const c_char,flags:i32,out:*mut i32)->i32);
delegate!(full_path,xFullPathname,(name:*const c_char,n:i32,out:*mut c_char)->i32);
delegate!(dl_open,xDlOpen,(name:*const c_char)->*mut c_void);
delegate!(dl_error,xDlError,(n:i32,out:*mut c_char)->());
delegate!(dl_sym,xDlSym,(handle:*mut c_void,name:*const c_char)->Option<unsafe extern "C" fn(*mut sqlite3_vfs,*mut c_void,*const c_char)>);
delegate!(dl_close,xDlClose,(handle:*mut c_void)->());
delegate!(randomness,xRandomness,(n:i32,out:*mut c_char)->i32);
delegate!(sleep,xSleep,(microseconds:i32)->i32);
delegate!(current_time,xCurrentTime,(out:*mut f64)->i32);
delegate!(last_error,xGetLastError,(n:i32,out:*mut c_char)->i32);
delegate!(current_time64,xCurrentTimeInt64,(out:*mut i64)->i32);
delegate!(set_system_call,xSetSystemCall,(name:*const c_char,call:sqlite3_syscall_ptr)->i32);
delegate!(get_system_call,xGetSystemCall,(name:*const c_char)->sqlite3_syscall_ptr);
delegate!(next_system_call,xNextSystemCall,(name:*const c_char)->*const c_char);

#[cfg(test)]
#[path = "../tests/unit/vfs.rs"]
mod tests;
