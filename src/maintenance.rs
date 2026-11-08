//! Deferred repair owns its SQLite connections. VFS callbacks only record work.
use crate::{
    config::BLOCK_SIZE,
    error::{Error, Result},
    ffi,
};
use std::{
    cell::Cell,
    collections::{BTreeMap, BTreeSet},
    ffi::{CStr, CString},
    panic::{AssertUnwindSafe, catch_unwind},
    ptr,
    sync::{
        Arc, Condvar, Mutex, MutexGuard, Weak,
        atomic::{AtomicUsize, Ordering},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

const QUEUE_LIMIT: usize = 4096;
const BATCH_LIMIT: usize = 16;
const RETRY_DELAY: Duration = Duration::from_millis(250);
const RETRY_LIMIT: u32 = 6;
// Observations share one short window before taking SQLite's write lock.
const BATCH_DELAY: Duration = Duration::from_millis(25);
const PREFIX_INTERVAL: Duration = Duration::from_secs(300);
thread_local! { static WORKER: Cell<bool> = const { Cell::new(false) }; }

pub fn is_worker() -> bool {
    WORKER.with(Cell::get)
}
fn lock<T>(value: &Mutex<T>) -> MutexGuard<'_, T> {
    value.lock().expect("maintenance mutex poisoned")
}
// False means this handle is unavailable; an attempted check returns true or an error.
pub type PrefixCheck = Arc<dyn Fn() -> Result<bool> + Send + Sync>;

#[derive(Clone, Debug)]
pub struct Status {
    pub stopped: bool,
    pub active: bool,
    pub blocked: bool,
    pub applications: usize,
    pub pending_pages: usize,
    pub committed_replacements: usize,
    pub main_damage: usize,
    pub wal_damage: usize,
    pub prefix_pending: usize,
    pub prefix_degraded: usize,
    pub queue_full: bool,
    pub zero_slot_reads: u64,
    pub wal_cutoffs: u64,
    pub wal_cutoff_bytes: u64,
    pub wal_cutoff_commits: u64,
    pub wal_last_cutoff: Option<u64>,
    pub wal_refusals: u64,
    pub main_sync_failures: u64,
    pub last_error: Option<Error>,
}
impl Status {
    pub fn json(&self) -> String {
        let error = match &self.last_error {
            Some(error) => format!(
                "{{\"code\":{},\"message\":{}}}",
                error.code,
                json_string(&error.message)
            ),
            None => "null".into(),
        };
        let last_cutoff = self
            .wal_last_cutoff
            .map_or("null".into(), |offset| offset.to_string());
        format!(
            concat!(
                "{{\"stopped\":{},\"active\":{},\"blocked\":{},",
                "\"applications\":{},\"pending_pages\":{},\"committed_replacements\":{},",
                "\"main_damage\":{},\"wal_damage\":{},\"prefix_pending\":{},",
                "\"prefix_degraded\":{},\"queue_full\":{},\"zero_slot_reads\":{},",
                "\"wal_cutoffs\":{},\"wal_cutoff_bytes\":{},\"wal_cutoff_commits\":{},",
                "\"wal_last_cutoff\":{},\"wal_refusals\":{},\"main_sync_failures\":{},\"last_error\":{}}}"
            ),
            self.stopped,
            self.active,
            self.blocked,
            self.applications,
            self.pending_pages,
            self.committed_replacements,
            self.main_damage,
            self.wal_damage,
            self.prefix_pending,
            self.prefix_degraded,
            self.queue_full,
            self.zero_slot_reads,
            self.wal_cutoffs,
            self.wal_cutoff_bytes,
            self.wal_cutoff_commits,
            last_cutoff,
            self.wal_refusals,
            self.main_sync_failures,
            error
        )
    }
}
fn json_string(text: &str) -> String {
    let mut result = String::from("\"");
    for ch in text.chars() {
        match ch {
            '"' => result.push_str("\\\""),
            '\\' => result.push_str("\\\\"),
            '\n' => result.push_str("\\n"),
            '\r' => result.push_str("\\r"),
            '\t' => result.push_str("\\t"),
            ch if ch < ' ' => {
                use std::fmt::Write;
                let _ = write!(result, "\\u{:04x}", ch as u32);
            }
            ch => result.push(ch),
        }
    }
    result.push('"');
    result
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PageState {
    Pending,
    Running,
    Committed,
    Failed,
}
struct Page {
    observation: [u8; 16],
    state: PageState,
    due: Instant,
    attempts: u32,
}
struct Prefix {
    handles: BTreeMap<u64, PrefixCheck>,
    due: Instant,
    degraded: bool,
    revision: u64,
}
struct Work {
    pages: BTreeMap<u64, Page>,
    batch_due: Option<Instant>,
    wal: BTreeSet<([u8; 16], u64)>,
    prefixes: BTreeMap<[u8; 16], Prefix>,
    next_token: u64,
    active: bool,
    blocked: bool,
    queue_full: bool,
    zero_slot_reads: u64,
    wal_cutoffs: u64,
    wal_cutoff_bytes: u64,
    wal_cutoff_commits: u64,
    wal_last_cutoff: Option<u64>,
    wal_refusals: u64,
    main_sync_failures: u64,
    last_error: Option<Error>,
}
impl Work {
    fn clear_empty_batch(&mut self) {
        if let Some(due) = self.batch_due
            && !self
                .pages
                .values()
                .any(|page| page.state == PageState::Pending && page.due == due)
        {
            self.batch_due = None;
        }
    }
    fn refresh_queue_full(&mut self) {
        self.queue_full &= self.pages.len() >= QUEUE_LIMIT || self.wal.len() >= QUEUE_LIMIT;
    }
}
pub struct Database {
    service: Weak<Service>,
    path: CString,
    id: [u8; 16],
    applications: AtomicUsize,
    writable_applications: AtomicUsize,
    cancellation: AtomicUsize,
    work: Mutex<Work>,
}

impl Database {
    pub fn path(&self) -> &CStr {
        &self.path
    }
    pub fn observe_zero_slots(&self, count: u64) {
        if count != 0 {
            lock(&self.work).zero_slot_reads += count;
        }
    }
    pub fn record_cutoff(&self, offset: u64, bytes: u64, commits: u64) {
        let mut work = lock(&self.work);
        work.wal_cutoffs += 1;
        work.wal_cutoff_bytes += bytes;
        work.wal_cutoff_commits += commits;
        work.wal_last_cutoff = Some(offset);
    }
    pub fn record_refusal(&self) {
        lock(&self.work).wal_refusals += 1;
    }
    pub fn record_main_sync_failure(&self) {
        lock(&self.work).main_sync_failures += 1;
    }
    pub fn application_open(&self, writable: bool) {
        let Some(service) = self.service.upgrade() else {
            return;
        };
        let mut state = lock(&service.state);
        // Match shutdown's lock order and publish both lifetime counts together.
        let _work = lock(&self.work);
        self.applications.fetch_add(1, Ordering::AcqRel);
        if writable {
            self.writable_applications.fetch_add(1, Ordering::AcqRel);
        }
        state.applications += 1;
        state.epoch = state.epoch.wrapping_add(1);
        service.wakeup.notify_all();
    }
    pub fn application_close(&self, writable: bool) -> Option<u64> {
        let service = self.service.upgrade()?;
        // Poison still permits cancelling and balancing a lifetime, never new work.
        let mut state = service.state.lock().unwrap_or_else(|poisoned| {
            let mut state = poisoned.into_inner();
            state.stopped = true;
            state.error = Some(Error::new(ffi::SQLITE_ERROR, "maintenance state poisoned"));
            state
        });
        let mut work = self.work.lock().unwrap_or_else(|poisoned| {
            state.stopped = true;
            state.error = Some(Error::new(ffi::SQLITE_ERROR, "maintenance work poisoned"));
            poisoned.into_inner()
        });
        // Worker handles never count; readers do not authorize queued writes.
        let closed = self
            .applications
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| n.checked_sub(1))
            .is_ok();
        if closed
            && writable
            && self
                .writable_applications
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| n.checked_sub(1))
                == Ok(1)
        {
            self.cancellation.fetch_add(1, Ordering::AcqRel);
            work.pages.clear();
            work.batch_due = None;
            work.refresh_queue_full();
            work.blocked = false;
        }
        if closed {
            state.applications -= 1;
        }
        state.epoch = state.epoch.wrapping_add(1);
        service.wakeup.notify_all();
        (state.applications == 0 && state.active).then_some(state.operation)
    }
    pub fn observe_main(&self, block: u64, observation: [u8; 16]) {
        let mut work = lock(&self.work);
        if work
            .pages
            .get(&block)
            .is_some_and(|page| page.observation == observation)
        {
            return;
        }
        if !work.pages.contains_key(&block) && work.pages.len() >= QUEUE_LIMIT {
            work.queue_full = true;
            return;
        }
        let due = *work
            .batch_due
            .get_or_insert_with(|| Instant::now() + BATCH_DELAY);
        work.pages.insert(
            block,
            Page {
                observation,
                state: PageState::Pending,
                due,
                attempts: 0,
            },
        );
        drop(work);
        self.wake();
    }
    /// Only successful physical replacement, removal, or revalidation retires damage.
    pub fn retire_main(&self, block: u64) {
        let mut work = lock(&self.work);
        work.pages.remove(&block);
        work.clear_empty_batch();
        work.refresh_queue_full();
    }
    pub fn retire_main_from(&self, from_block: u64) {
        let mut work = lock(&self.work);
        work.pages.retain(|block, _| *block < from_block);
        work.clear_empty_batch();
        work.refresh_queue_full();
    }
    pub fn observe_wal(&self, file_id: [u8; 16], block: u64) {
        let mut work = lock(&self.work);
        if work.wal.len() >= QUEUE_LIMIT && !work.wal.contains(&(file_id, block)) {
            work.queue_full = true;
        } else {
            work.wal.insert((file_id, block));
        }
    }
    pub fn retire_wal_block(&self, file_id: [u8; 16], block: u64) {
        let mut work = lock(&self.work);
        work.wal.remove(&(file_id, block));
        work.refresh_queue_full();
    }
    /// Called after a successful truncation, not merely WAL restart or reuse.
    pub fn retire_wal(&self, file_id: [u8; 16], from_block: u64) {
        let mut work = lock(&self.work);
        work.wal
            .retain(|(id, block)| *id != file_id || *block < from_block);
        work.refresh_queue_full();
    }
    pub fn register_prefix(&self, file_id: [u8; 16], check: PrefixCheck) -> u64 {
        let mut work = lock(&self.work);
        let token = work.next_token;
        work.next_token = work
            .next_token
            .checked_add(1)
            .expect("prefix token exhausted");
        let prefix = work.prefixes.entry(file_id).or_insert_with(|| Prefix {
            handles: BTreeMap::new(),
            due: Instant::now() + PREFIX_INTERVAL,
            degraded: false,
            revision: 0,
        });
        prefix.handles.insert(token, check);
        drop(work);
        self.wake();
        token
    }
    pub fn unregister_prefix(&self, token: u64) {
        let mut work = lock(&self.work);
        work.prefixes.retain(|_, prefix| {
            prefix.handles.remove(&token);
            !prefix.handles.is_empty()
        });
        drop(work);
        self.wake();
    }
    pub fn observe_prefix(&self, file_id: [u8; 16]) {
        if let Some(prefix) = lock(&self.work).prefixes.get_mut(&file_id) {
            prefix.degraded = true;
            prefix.due = Instant::now();
            prefix.revision = prefix.revision.wrapping_add(1);
        }
        self.wake();
    }
    pub fn snapshot(&self) -> Status {
        let service = self.service.upgrade();
        let (stopped, service_error) = match &service {
            Some(service) => {
                let state = lock(&service.state);
                (state.stopped, state.error.clone())
            }
            None => (true, None),
        };
        let work = lock(&self.work);
        Status {
            stopped,
            active: work.active,
            blocked: work.blocked,
            applications: self.applications.load(Ordering::Acquire),
            pending_pages: work
                .pages
                .values()
                .filter(|p| matches!(p.state, PageState::Pending | PageState::Running))
                .count(),
            committed_replacements: work
                .pages
                .values()
                .filter(|p| p.state == PageState::Committed)
                .count(),
            main_damage: work.pages.len(),
            wal_damage: work.wal.len(),
            prefix_pending: work
                .prefixes
                .values()
                .filter(|p| p.due <= Instant::now())
                .count(),
            prefix_degraded: work.prefixes.values().filter(|p| p.degraded).count(),
            queue_full: work.queue_full,
            zero_slot_reads: work.zero_slot_reads,
            wal_cutoffs: work.wal_cutoffs,
            wal_cutoff_bytes: work.wal_cutoff_bytes,
            wal_cutoff_commits: work.wal_cutoff_commits,
            wal_last_cutoff: work.wal_last_cutoff,
            wal_refusals: work.wal_refusals,
            main_sync_failures: work.main_sync_failures,
            last_error: work.last_error.clone().or(service_error),
        }
    }
    fn wake(&self) {
        if let Some(service) = self.service.upgrade() {
            service.wake();
        }
    }
}

trait RepairBackend: Send + Sync {
    fn cleanup_error(&self) -> Option<Error> {
        None
    }
    /// true means the current page was rewritten; false means it no longer exists.
    fn rewrite(
        &self,
        database: &Database,
        blocks: &[u64],
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<bool>>;
}
struct ServiceState {
    databases: Vec<Weak<Database>>,
    applications: usize,
    active: bool,
    operation: u64,
    epoch: u64,
    stopped: bool,
    finished: bool,
    error: Option<Error>,
}
pub struct Service {
    backend: Arc<dyn RepairBackend>,
    state: Mutex<ServiceState>,
    wakeup: Condvar,
    worker: Mutex<Option<JoinHandle<()>>>,
}
// Includes prefix I/O and the entire lifetime of a private SQLite connection.
struct Operation<'a>(&'a Service);
impl Drop for Operation<'_> {
    fn drop(&mut self) {
        let mut state = self
            .0
            .state
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        state.active = false;
        self.0.wakeup.notify_all();
    }
}
impl Service {
    pub fn new(api: Arc<ffi::Api>, vfs_name: CString) -> Arc<Self> {
        Self::with_backend(Arc::new(SqliteBackend {
            api,
            vfs_name,
            cleanup_error: Mutex::new(None),
        }))
    }
    fn with_backend(backend: Arc<dyn RepairBackend>) -> Arc<Self> {
        let service = Arc::new(Self {
            backend,
            state: Mutex::new(ServiceState {
                databases: Vec::new(),
                applications: 0,
                active: false,
                operation: 0,
                epoch: 0,
                stopped: false,
                finished: false,
                error: None,
            }),
            wakeup: Condvar::new(),
            worker: Mutex::new(None),
        });
        let owner = service.clone();
        match thread::Builder::new()
            .name("monstervfs-repair".into())
            .spawn(move || {
                WORKER.with(|worker| worker.set(true));
                let outcome = catch_unwind(AssertUnwindSafe(|| owner.run()));
                // A poisoned state may be inspected only to finish terminal cleanup.
                let mut state = owner
                    .state
                    .lock()
                    .unwrap_or_else(|error| error.into_inner());
                if outcome.is_err() {
                    state.error =
                        Some(Error::new(ffi::SQLITE_ERROR, "maintenance worker panicked"));
                    state.stopped = true;
                }
                state.finished = true;
                owner.wakeup.notify_all();
            }) {
            Ok(worker) => *lock(&service.worker) = Some(worker),
            Err(error) => {
                let mut state = lock(&service.state);
                state.stopped = true;
                state.finished = true;
                state.error = Some(Error::new(
                    ffi::SQLITE_ERROR,
                    format!("maintenance worker: {error}"),
                ));
            }
        }
        service
    }
    pub fn register_database(self: &Arc<Self>, path: CString, id: [u8; 16]) -> Arc<Database> {
        let mut state = lock(&self.state);
        state.databases.retain(|entry| entry.strong_count() != 0);
        if let Some(database) = state
            .databases
            .iter()
            .filter_map(Weak::upgrade)
            .find(|db| db.id == id && db.path == path)
        {
            return database;
        }
        let database = Arc::new(Database {
            service: Arc::downgrade(self),
            path,
            id,
            applications: AtomicUsize::new(0),
            writable_applications: AtomicUsize::new(0),
            cancellation: AtomicUsize::new(0),
            work: Mutex::new(Work {
                pages: BTreeMap::new(),
                batch_due: None,
                wal: BTreeSet::new(),
                prefixes: BTreeMap::new(),
                next_token: 1,
                active: false,
                blocked: false,
                queue_full: false,
                zero_slot_reads: 0,
                wal_cutoffs: 0,
                wal_cutoff_bytes: 0,
                wal_cutoff_commits: 0,
                wal_last_cutoff: None,
                wal_refusals: 0,
                main_sync_failures: 0,
                last_error: None,
            }),
        });
        state.databases.push(Arc::downgrade(&database));
        database
    }
    fn operation(&self) -> Option<Operation<'_>> {
        let mut state = lock(&self.state);
        if state.stopped || state.applications == 0 {
            return None;
        }
        debug_assert!(!state.active);
        state.active = true;
        state.operation = state.operation.wrapping_add(1);
        Some(Operation(self))
    }
    /// Actual connection teardown reaches this after SQLite releases its Btrees.
    /// Replacing the private function also withdraws authorization; private-cache
    /// worker connections have bounded busy waits for that connection's locks.
    /// Reopening never waits here, and does not extend an older close's barrier.
    pub fn wait_for_idle(&self, operation: Option<u64>) {
        let Some(operation) = operation else { return };
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        while state.active && state.operation == operation {
            state = self
                .wakeup
                .wait(state)
                .unwrap_or_else(|error| error.into_inner());
        }
    }
    pub fn shutdown(&self) -> Result<()> {
        if is_worker() {
            return Err(Error::new(
                ffi::SQLITE_MISUSE,
                "maintenance shutdown called from worker",
            ));
        }
        {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| {
                let mut state = poisoned.into_inner();
                state.error = Some(Error::new(ffi::SQLITE_ERROR, "maintenance state poisoned"));
                state
            });
            state.stopped = true;
            let databases: Vec<_> = state.databases.iter().filter_map(Weak::upgrade).collect();
            for database in databases {
                database.cancellation.fetch_add(1, Ordering::AcqRel);
                match database.work.lock() {
                    Ok(mut work) => {
                        work.pages.clear();
                        work.batch_due = None;
                    }
                    Err(_) => {
                        state.error =
                            Some(Error::new(ffi::SQLITE_ERROR, "maintenance work poisoned"))
                    }
                }
            }
            self.wakeup.notify_all();
        }
        // Taking a poisoned handle here permits joining, never restarting work.
        if let Some(worker) = self
            .worker
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .take()
        {
            let _ = worker.join();
        }
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        while !state.finished {
            state = self.wakeup.wait(state).unwrap_or_else(|e| e.into_inner());
        }
        match &state.error {
            Some(error) => Err(error.clone()),
            None => Ok(()),
        }
    }
    fn wake(&self) {
        let mut state = lock(&self.state);
        state.epoch = state.epoch.wrapping_add(1);
        self.wakeup.notify_all();
    }
    fn run(&self) {
        loop {
            let (databases, epoch) = {
                let state = lock(&self.state);
                if state.stopped {
                    return;
                }
                (
                    state
                        .databases
                        .iter()
                        .filter_map(Weak::upgrade)
                        .collect::<Vec<_>>(),
                    state.epoch,
                )
            };
            let mut due = Instant::now() + PREFIX_INTERVAL;
            let mut ran = false;
            for database in databases {
                if lock(&self.state).stopped {
                    return;
                }
                if database.writable_applications.load(Ordering::Acquire) == 0 {
                    continue;
                }
                let Some(_operation) = self.operation() else {
                    continue;
                };
                // Prefix checks stay independent of a busy SQLite writer.
                if self.check_prefix(&database) {
                    ran = true;
                }
                if self.rewrite_pages(&database) {
                    ran = true;
                }
                if let Some(error) = self.backend.cleanup_error() {
                    let mut state = lock(&self.state);
                    state.error = Some(error);
                    state.stopped = true;
                    return;
                }
                let work = lock(&database.work);
                for prefix in work.prefixes.values() {
                    due = due.min(prefix.due);
                }
                for page in work
                    .pages
                    .values()
                    .filter(|page| page.state == PageState::Pending)
                {
                    due = due.min(page.due);
                }
            }
            if ran {
                continue;
            }
            let state = lock(&self.state);
            if state.stopped {
                return;
            }
            if state.epoch != epoch {
                continue;
            }
            let _ = self
                .wakeup
                .wait_timeout(state, due.saturating_duration_since(Instant::now()))
                .expect("maintenance mutex poisoned while waiting");
        }
    }
    fn check_prefix(&self, database: &Database) -> bool {
        let (id, revision, handles) = {
            let mut work = lock(&database.work);
            let Some((id, prefix)) = work
                .prefixes
                .iter_mut()
                .find(|(_, p)| p.due <= Instant::now())
            else {
                return false;
            };
            prefix.due = Instant::now() + PREFIX_INTERVAL;
            (
                *id,
                prefix.revision,
                prefix
                    .handles
                    .iter()
                    .map(|(&token, check)| (token, check.clone()))
                    .collect::<Vec<_>>(),
            )
        };
        for (token, check) in handles {
            if database.writable_applications.load(Ordering::Acquire) == 0
                || lock(&self.state).stopped
            {
                break;
            }
            let result = check();
            if matches!(result, Ok(false)) {
                continue;
            }
            let mut work = lock(&database.work);
            if let Some(prefix) = work.prefixes.get_mut(&id)
                && prefix.handles.contains_key(&token)
                && prefix.revision == revision
            {
                prefix.degraded = result.is_err();
                if let Err(error) = result {
                    work.last_error = Some(error);
                }
            }
            break;
        }
        true
    }
    fn rewrite_pages(&self, database: &Database) -> bool {
        let (jobs, epoch) = {
            let mut work = lock(&database.work);
            let now = Instant::now();
            if work.batch_due.is_some_and(|due| due <= now) {
                work.batch_due = None;
            }
            let jobs: Vec<_> = work
                .pages
                .iter_mut()
                .filter(|(_, p)| p.state == PageState::Pending && p.due <= now)
                .take(BATCH_LIMIT)
                .map(|(&block, page)| {
                    page.state = PageState::Running;
                    (block, page.observation)
                })
                .collect();
            if jobs.is_empty() {
                return false;
            }
            work.active = true;
            work.blocked = false;
            (jobs, database.cancellation.load(Ordering::Acquire))
        };
        let cancelled = || {
            database.writable_applications.load(Ordering::Acquire) == 0
                || database.cancellation.load(Ordering::Acquire) != epoch
                || lock(&self.state).stopped
        };
        let blocks = jobs.iter().map(|job| job.0).collect::<Vec<_>>();
        let attempt = |blocks: &[u64]| {
            if cancelled() {
                Err(Error::new(ffi::SQLITE_ABORT, "maintenance cancelled"))
            } else if let Some(error) = self.backend.cleanup_error() {
                Err(error)
            } else {
                self.backend.rewrite(database, blocks, &cancelled)
            }
        };
        let result = attempt(&blocks);
        // One unreadable page does not establish that its batch neighbors are
        // unreadable too. Retry them separately after the failed batch rolls back.
        let isolated = (blocks.len() > 1
            && result
                .as_ref()
                .is_err_and(|error| error.code == crate::error::IOERR_DATA))
        .then(|| {
            blocks
                .iter()
                .map(|block| attempt(std::slice::from_ref(block)))
                .collect::<Vec<_>>()
        });
        let was_cancelled = cancelled();
        let mut work = lock(&database.work);
        work.active = false;
        if was_cancelled {
            return true;
        }
        work.blocked = false;
        for (index, (block, observation)) in jobs.iter().enumerate() {
            let (result, index) = isolated
                .as_ref()
                .map_or((&result, index), |results| (&results[index], 0));
            let busy = result.as_ref().err().is_some_and(|error| {
                matches!(error.code & 0xff, ffi::SQLITE_BUSY | ffi::SQLITE_LOCKED)
            });
            work.blocked |= busy;
            if let Err(error) = result {
                work.last_error = Some(error.clone());
            }
            let Some(page) = work
                .pages
                .get_mut(block)
                .filter(|p| p.observation == *observation && p.state == PageState::Running)
            else {
                continue;
            };
            match result {
                Ok(updated) if updated.get(index) == Some(&true) => {
                    page.state = PageState::Committed
                }
                Ok(updated) if updated.get(index) == Some(&false) => {
                    work.pages.remove(block);
                }
                Ok(_) => {
                    page.state = PageState::Failed;
                    work.last_error = Some(Error::new(
                        ffi::SQLITE_ERROR,
                        "maintenance result count is different",
                    ));
                }
                Err(error) => {
                    // Damage stays damaged. Anything else can clear by itself.
                    let transient = busy || error.code != crate::error::IOERR_DATA;
                    page.attempts += !busy as u32;
                    page.state = if transient && page.attempts < RETRY_LIMIT {
                        PageState::Pending
                    } else {
                        PageState::Failed
                    };
                    page.due = Instant::now() + RETRY_DELAY * (1 << page.attempts);
                }
            }
        }
        true
    }
}

struct SqliteBackend {
    api: Arc<ffi::Api>,
    vfs_name: CString,
    cleanup_error: Mutex<Option<Error>>,
}
struct Connection<'a> {
    api: &'a ffi::Api,
    raw: *mut ffi::sqlite3,
}
impl Connection<'_> {
    fn error(&self, fallback: i32) -> Error {
        let code = unsafe { self.api.extended_errcode(self.raw) };
        let message = unsafe { CStr::from_ptr(self.api.errmsg(self.raw)) }.to_string_lossy();
        Error::new(
            if code == ffi::SQLITE_OK {
                fallback
            } else {
                code
            },
            message,
        )
    }
    fn check(&self, code: i32) -> Result<()> {
        if code == ffi::SQLITE_OK {
            Ok(())
        } else {
            Err(self.error(code))
        }
    }
    fn exec(&self, sql: &CStr) -> Result<()> {
        self.check(unsafe { self.api.exec(self.raw, sql.as_ptr(), ptr::null_mut()) })
    }
    fn prepare<'a>(&'a self, sql: &CStr) -> Result<Statement<'a>> {
        let mut raw = ptr::null_mut();
        let code = unsafe {
            self.api
                .prepare_v2(self.raw, sql.as_ptr(), -1, &mut raw, ptr::null_mut())
        };
        if code != ffi::SQLITE_OK {
            if !raw.is_null() {
                unsafe { self.api.finalize(raw) };
            }
            return Err(self.error(code));
        }
        Ok(Statement {
            connection: self,
            raw,
        })
    }
    fn close(mut self) -> Result<()> {
        let raw = self.raw;
        self.raw = ptr::null_mut();
        let code = unsafe { self.api.close(raw) };
        if code == ffi::SQLITE_OK {
            Ok(())
        } else {
            Err(Error::new(code, "maintenance connection close failed"))
        }
    }
}
impl Drop for Connection<'_> {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { self.api.close(self.raw) };
        }
    }
}
struct Statement<'a> {
    connection: &'a Connection<'a>,
    raw: *mut ffi::sqlite3_stmt,
}
impl Drop for Statement<'_> {
    fn drop(&mut self) {
        unsafe { self.connection.api.finalize(self.raw) };
    }
}
impl RepairBackend for SqliteBackend {
    fn cleanup_error(&self) -> Option<Error> {
        lock(&self.cleanup_error).clone()
    }
    fn rewrite(
        &self,
        database: &Database,
        blocks: &[u64],
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<bool>> {
        let mut raw = ptr::null_mut();
        let code = unsafe {
            self.api.open_v2(
                database.path.as_ptr(),
                &mut raw,
                ffi::SQLITE_OPEN_READWRITE
                    | ffi::SQLITE_OPEN_PRIVATECACHE
                    | ffi::SQLITE_OPEN_FULLMUTEX,
                self.vfs_name.as_ptr(),
            )
        };
        if raw.is_null() {
            return Err(Error::new(code, "maintenance connection open failed"));
        }
        let connection = Connection {
            api: &self.api,
            raw,
        };
        // Install this even on a failed open, before the owned handle can close.
        let no_checkpoint = connection.check(unsafe {
            self.api.db_config_int(
                raw,
                ffi::SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE,
                1,
                ptr::null_mut(),
            )
        });
        let result = (|| {
            if code != ffi::SQLITE_OK {
                return Err(connection.error(code));
            }
            no_checkpoint?;
            connection.check(unsafe { self.api.wal_autocheckpoint(raw, 0) })?;
            connection.check(unsafe { self.api.busy_timeout(raw, 50) })?;
            connection.check(unsafe {
                self.api
                    .db_config_int(raw, ffi::SQLITE_DBCONFIG_DEFENSIVE, 0, ptr::null_mut())
            })?;
            if cancelled() {
                return Err(Error::new(ffi::SQLITE_ABORT, "maintenance cancelled"));
            }
            let mut opened_id = [0u8; 16];
            connection.check(unsafe {
                self.api.file_control(
                    raw,
                    c"main".as_ptr(),
                    crate::vfs::FCNTL_ID,
                    opened_id.as_mut_ptr().cast(),
                )
            })?;
            if opened_id != database.id {
                return Err(Error::corrupt("maintenance database identity is different"));
            }
            {
                let statement = connection.prepare(c"PRAGMA journal_mode")?;
                let code = unsafe { self.api.step(statement.raw) };
                if code != ffi::SQLITE_ROW {
                    return Err(connection.error(code));
                }
                let mode = unsafe { self.api.column_text(statement.raw, 0) };
                if mode.is_null() || unsafe { CStr::from_ptr(mode.cast()) }.to_bytes() != b"wal" {
                    return Err(Error::new(
                        ffi::SQLITE_ERROR,
                        "maintenance requires WAL mode",
                    ));
                }
            }
            connection.exec(c"PRAGMA synchronous=FULL")?;
            {
                let statement = connection.prepare(c"PRAGMA page_size")?;
                let code = unsafe { self.api.step(statement.raw) };
                if code != ffi::SQLITE_ROW {
                    return Err(connection.error(code));
                }
                if unsafe { self.api.column_int64(statement.raw, 0) } != BLOCK_SIZE as i64 {
                    return Err(Error::corrupt("maintenance page size is different"));
                }
            }
            connection.exec(c"BEGIN IMMEDIATE")?;
            let transaction = (|| {
                let mut id = [0u8; 16];
                connection.check(unsafe {
                    self.api.file_control(
                        raw,
                        c"main".as_ptr(),
                        crate::vfs::FCNTL_ID,
                        id.as_mut_ptr().cast(),
                    )
                })?;
                if id != database.id {
                    return Err(Error::corrupt("maintenance database identity is different"));
                }
                let mut updated = Vec::with_capacity(blocks.len());
                let statement = connection.prepare(
                    c"UPDATE sqlite_dbpage SET data=data WHERE schema='main' AND pgno=?1",
                )?;
                for &block in blocks {
                    if cancelled() {
                        return Err(Error::new(ffi::SQLITE_ABORT, "maintenance cancelled"));
                    }
                    let page = block
                        .checked_add(1)
                        .filter(|page| *page <= u32::MAX as u64)
                        .ok_or_else(|| Error::corrupt("maintenance page number is illegal"))?;
                    connection
                        .check(unsafe { self.api.bind_int64(statement.raw, 1, page as i64) })?;
                    let code = unsafe { self.api.step(statement.raw) };
                    if code != ffi::SQLITE_DONE {
                        return Err(connection.error(code));
                    }
                    updated.push(unsafe { self.api.changes(raw) } != 0);
                    connection.check(unsafe { self.api.reset(statement.raw) })?;
                }
                drop(statement);
                if cancelled() {
                    return Err(Error::new(ffi::SQLITE_ABORT, "maintenance cancelled"));
                }
                connection.exec(c"COMMIT")?;
                Ok(updated)
            })();
            if transaction.is_err() && unsafe { self.api.get_autocommit(raw) } == 0 {
                let _ = connection.exec(c"ROLLBACK");
            }
            transaction
        })();
        let closed = connection.close();
        if let Err(error) = &closed {
            *lock(&self.cleanup_error) = Some(error.clone());
        }
        match result {
            Err(error) => Err(error),
            Ok(updated) => closed.map(|()| updated),
        }
    }
}

#[cfg(test)]
#[path = "../tests/unit/maintenance.rs"]
mod tests;
