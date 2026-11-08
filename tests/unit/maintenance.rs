use super::*;
use std::sync::{atomic::AtomicBool, mpsc};

struct Backend {
    calls: Mutex<Vec<Vec<u64>>>,
    results: Mutex<std::collections::VecDeque<Result<Vec<bool>>>>,
}
impl Backend {
    fn new(results: Vec<Result<Vec<bool>>>) -> Arc<Self> {
        Arc::new(Self {
            calls: Mutex::new(Vec::new()),
            results: Mutex::new(results.into()),
        })
    }
}
impl RepairBackend for Backend {
    fn rewrite(
        &self,
        _database: &Database,
        blocks: &[u64],
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<bool>> {
        assert!(is_worker());
        assert!(!cancelled());
        lock(&self.calls).push(blocks.to_vec());
        lock(&self.results)
            .pop_front()
            .unwrap_or_else(|| Ok(vec![true; blocks.len()]))
    }
}
fn database(service: &Arc<Service>) -> Arc<Database> {
    service.register_database(CString::new("/a/database").unwrap(), [1; 16])
}
fn until(predicate: impl Fn() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(3);
    while !predicate() {
        assert!(Instant::now() < deadline, "maintenance condition timed out");
        thread::sleep(Duration::from_millis(1));
    }
}

// Drive scheduling explicitly in deadline tests; threaded tests below cover wakeups.
fn manual_service(backend: Arc<dyn RepairBackend>) -> Arc<Service> {
    Arc::new(Service {
        backend,
        state: Mutex::new(ServiceState {
            databases: Vec::new(),
            applications: 0,
            active: false,
            operation: 0,
            epoch: 0,
            stopped: false,
            finished: true,
            error: None,
        }),
        wakeup: Condvar::new(),
        worker: Mutex::new(None),
    })
}
fn run_batch(service: &Service, db: &Database) -> bool {
    WORKER.with(|worker| {
        assert!(!worker.replace(true));
        let ran = service.rewrite_pages(db);
        worker.set(false);
        ran
    })
}
fn set_batch_due(db: &Database, due: Instant) {
    let mut work = lock(&db.work);
    let old = work.batch_due.expect("queued observation window");
    for page in work.pages.values_mut() {
        if page.state == PageState::Pending && page.due == old {
            page.due = due;
        }
    }
    work.batch_due = Some(due);
}
fn expire_batch(db: &Database) {
    set_batch_due(db, Instant::now() - Duration::from_secs(1));
}

#[test]
fn observations_share_one_batch_window_without_extending_it() {
    let backend = Backend::new(Vec::new());
    let service = manual_service(backend.clone());
    let db = database(&service);
    db.application_open(true);
    let before = Instant::now();
    db.observe_main(8, [3; 16]);
    let after = Instant::now();
    let due = lock(&db.work).batch_due.unwrap();
    assert!(due >= before + BATCH_DELAY && due <= after + BATCH_DELAY);

    // A future deadline makes these assertions independent of host scheduling.
    let due = Instant::now() + Duration::from_secs(60);
    set_batch_due(&db, due);
    db.observe_main(2, [3; 16]);
    db.observe_main(8, [3; 16]);
    db.observe_main(7, [3; 16]);
    {
        let work = lock(&db.work);
        assert_eq!(work.batch_due, Some(due));
        assert!(work.pages.values().all(|page| page.due == due));
    }
    assert!(!run_batch(&service, &db));
    assert!(lock(&backend.calls).is_empty());
    expire_batch(&db);
    assert!(run_batch(&service, &db));
    assert_eq!(*lock(&backend.calls), vec![vec![2, 7, 8]]);
    assert_eq!(lock(&db.work).batch_due, None);
    assert!(!run_batch(&service, &db));
    service.shutdown().unwrap();
}

#[test]
fn due_work_drains_in_batches_of_sixteen_without_another_delay() {
    let backend = Backend::new(Vec::new());
    let service = manual_service(backend.clone());
    let db = database(&service);
    db.application_open(true);
    for block in 0..35 {
        db.observe_main(block, [3; 16]);
    }
    expire_batch(&db);
    for remaining in [19, 3, 0] {
        assert!(run_batch(&service, &db));
        assert_eq!(db.snapshot().pending_pages, remaining);
    }
    assert!(!run_batch(&service, &db));
    assert_eq!(
        *lock(&backend.calls),
        vec![
            (0..16).collect::<Vec<_>>(),
            (16..32).collect(),
            (32..35).collect()
        ]
    );
    assert_eq!(db.snapshot().committed_replacements, 35);
    service.shutdown().unwrap();
}

#[test]
fn unreadable_batch_isolates_good_removed_and_busy_pages() {
    let backend = Backend::new(vec![
        Err(Error::corrupt("one batch page is unreadable")),
        Ok(vec![true]),
        Err(Error::corrupt("page two is unreadable")),
        Ok(vec![true]),
        Ok(vec![false]),
        Err(Error::new(ffi::SQLITE_BUSY, "writer busy")),
    ]);
    let service = manual_service(backend.clone());
    let db = database(&service);
    db.application_open(true);
    for block in 1..=5 {
        db.observe_main(block, [3; 16]);
    }
    expire_batch(&db);
    let before = Instant::now();
    assert!(run_batch(&service, &db));
    assert_eq!(
        *lock(&backend.calls),
        vec![
            vec![1, 2, 3, 4, 5],
            vec![1],
            vec![2],
            vec![3],
            vec![4],
            vec![5]
        ]
    );
    {
        let work = lock(&db.work);
        assert!(work.pages[&1].state == PageState::Committed);
        assert!(work.pages[&2].state == PageState::Failed);
        assert!(work.pages[&3].state == PageState::Committed);
        assert!(!work.pages.contains_key(&4));
        assert!(work.pages[&5].state == PageState::Pending);
        assert_eq!(work.pages[&5].attempts, 0);
        assert!(work.pages[&5].due >= before + RETRY_DELAY);
    }
    assert_eq!(db.snapshot().committed_replacements, 2);
    assert_eq!(db.snapshot().pending_pages, 1);
    assert!(db.snapshot().blocked);
    // Rediscovering unchanged damage neither repeats success nor retries the bad page.
    for block in [1, 2, 3, 5] {
        db.observe_main(block, [3; 16]);
    }
    lock(&db.work).pages.get_mut(&5).unwrap().due = Instant::now() - Duration::from_secs(1);
    assert!(run_batch(&service, &db));
    assert_eq!(lock(&backend.calls).last(), Some(&vec![5]));
    assert_eq!(lock(&backend.calls).len(), 7);
    assert_eq!(db.snapshot().committed_replacements, 3);
    assert!(!db.snapshot().blocked);
    service.shutdown().unwrap();
}

#[test]
fn other_batch_errors_do_not_start_single_page_retries() {
    for code in [
        ffi::SQLITE_BUSY,
        ffi::SQLITE_CANTOPEN,
        ffi::SQLITE_IOERR_FSYNC,
    ] {
        let backend = Backend::new(vec![Err(Error::new(code, "batch failed"))]);
        let service = manual_service(backend.clone());
        let db = database(&service);
        db.application_open(true);
        db.observe_main(1, [3; 16]);
        db.observe_main(2, [3; 16]);
        expire_batch(&db);
        assert!(run_batch(&service, &db));
        assert_eq!(*lock(&backend.calls), vec![vec![1, 2]]);
        assert_eq!(db.snapshot().pending_pages, 2);
        assert_eq!(db.snapshot().committed_replacements, 0);
        service.shutdown().unwrap();
    }
}

#[derive(Clone, Copy)]
enum DuringFallback {
    Cancel,
    CleanupFailure,
    NewObservation,
}
struct FallbackBackend {
    calls: Mutex<Vec<Vec<u64>>>,
    action: DuringFallback,
    at_call: usize,
    cleanup_failed: AtomicBool,
}
impl RepairBackend for FallbackBackend {
    fn cleanup_error(&self) -> Option<Error> {
        self.cleanup_failed
            .load(Ordering::Acquire)
            .then(|| Error::new(ffi::SQLITE_IOERR, "owned connection close failed"))
    }
    fn rewrite(
        &self,
        database: &Database,
        blocks: &[u64],
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<bool>> {
        assert!(!cancelled());
        let count = {
            let mut calls = lock(&self.calls);
            calls.push(blocks.to_vec());
            calls.len()
        };
        if count == self.at_call {
            match self.action {
                DuringFallback::Cancel => {
                    database.application_close(true);
                }
                DuringFallback::CleanupFailure => {
                    self.cleanup_failed.store(true, Ordering::Release)
                }
                DuringFallback::NewObservation => {
                    database.observe_main(1, [4; 16]);
                    database.observe_main(3, [3; 16]);
                }
            }
        }
        if count == 1 {
            Err(Error::corrupt("batch page is unreadable"))
        } else if let Some(error) = self.cleanup_error() {
            Err(error)
        } else {
            Ok(vec![true; blocks.len()])
        }
    }
}
fn fallback_backend(action: DuringFallback, at_call: usize) -> Arc<FallbackBackend> {
    Arc::new(FallbackBackend {
        calls: Mutex::new(Vec::new()),
        action,
        at_call,
        cleanup_failed: AtomicBool::new(false),
    })
}
#[test]
fn fallback_checks_cancellation_and_cleanup_before_every_new_connection() {
    for action in [DuringFallback::Cancel, DuringFallback::CleanupFailure] {
        for at_call in [1, 2] {
            let backend = fallback_backend(action, at_call);
            let service = manual_service(backend.clone());
            let db = database(&service);
            db.application_open(true);
            for block in 1..=3 {
                db.observe_main(block, [3; 16]);
            }
            expire_batch(&db);
            assert!(run_batch(&service, &db));
            let expected = if at_call == 1 {
                vec![vec![1, 2, 3]]
            } else {
                vec![vec![1, 2, 3], vec![1]]
            };
            assert_eq!(*lock(&backend.calls), expected);
            assert_eq!(db.snapshot().committed_replacements, 0);
            assert!(!db.snapshot().active);
            match action {
                DuringFallback::Cancel => assert_eq!(db.snapshot().main_damage, 0),
                DuringFallback::CleanupFailure => {
                    assert_eq!(db.snapshot().pending_pages, 3);
                    assert_eq!(
                        db.snapshot().last_error.unwrap().message,
                        "owned connection close failed"
                    );
                }
                DuringFallback::NewObservation => unreachable!(),
            }
            service.shutdown().unwrap();
        }
    }
}

#[test]
fn a_new_observation_during_fallback_survives_the_old_result() {
    let backend = fallback_backend(DuringFallback::NewObservation, 2);
    let service = manual_service(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.observe_main(1, [3; 16]);
    db.observe_main(2, [3; 16]);
    expire_batch(&db);
    assert!(run_batch(&service, &db));
    {
        let work = lock(&db.work);
        assert_eq!(work.pages[&1].observation, [4; 16]);
        assert!(work.pages[&1].state == PageState::Pending);
        assert!(work.pages[&2].state == PageState::Committed);
        assert!(work.pages[&3].state == PageState::Pending);
        assert_eq!(work.pages[&1].due, work.batch_due.unwrap());
    }
    expire_batch(&db);
    assert!(run_batch(&service, &db));
    assert_eq!(
        *lock(&backend.calls),
        vec![vec![1, 2], vec![1], vec![2], vec![1, 3]]
    );
    assert_eq!(db.snapshot().committed_replacements, 3);
    service.shutdown().unwrap();
}

#[test]
fn retiring_or_cancelling_the_last_queued_page_resets_the_window() {
    let service = manual_service(Backend::new(Vec::new()));
    let db = database(&service);
    db.application_open(true);
    for truncate in [false, true] {
        db.observe_main(8, [3; 16]);
        expire_batch(&db);
        if truncate {
            db.retire_main_from(8);
        } else {
            db.retire_main(8);
        }
        assert_eq!(lock(&db.work).batch_due, None);
    }
    db.observe_main(8, [3; 16]);
    expire_batch(&db);
    db.application_close(true);
    assert_eq!(lock(&db.work).batch_due, None);
    db.application_open(true);
    let before = Instant::now();
    db.observe_main(8, [3; 16]);
    assert!(lock(&db.work).batch_due.unwrap() >= before + BATCH_DELAY);
    service.shutdown().unwrap();
}

#[test]
fn new_observations_do_not_skip_busy_retry_deadlines() {
    let backend = Backend::new(vec![Err(Error::new(ffi::SQLITE_BUSY, "writer busy"))]);
    let service = manual_service(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.observe_main(8, [3; 16]);
    expire_batch(&db);
    let before = Instant::now();
    assert!(run_batch(&service, &db));
    let after = Instant::now();
    let retry_due = lock(&db.work).pages[&8].due;
    assert!(retry_due >= before + RETRY_DELAY && retry_due <= after + RETRY_DELAY);
    assert_eq!(lock(&db.work).pages[&8].attempts, 0);
    assert!(db.snapshot().blocked);

    let retry_due = Instant::now() + Duration::from_secs(60);
    lock(&db.work).pages.get_mut(&8).unwrap().due = retry_due;
    db.observe_main(8, [3; 16]);
    db.observe_main(4, [3; 16]);
    expire_batch(&db);
    assert!(run_batch(&service, &db));
    assert_eq!(*lock(&backend.calls), vec![vec![8], vec![4]]);
    assert_eq!(lock(&db.work).pages[&8].due, retry_due);
    assert!(!run_batch(&service, &db));
    lock(&db.work).pages.get_mut(&8).unwrap().due = Instant::now() - Duration::from_secs(1);
    assert!(run_batch(&service, &db));
    assert_eq!(*lock(&backend.calls), vec![vec![8], vec![4], vec![8]]);
    service.shutdown().unwrap();
}

struct ObservingBackend {
    calls: Mutex<Vec<Vec<u64>>>,
}
impl RepairBackend for ObservingBackend {
    fn rewrite(
        &self,
        database: &Database,
        blocks: &[u64],
        _cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<bool>> {
        let mut calls = lock(&self.calls);
        calls.push(blocks.to_vec());
        if calls.len() == 1 {
            database.observe_main(8, [4; 16]);
            database.observe_main(9, [3; 16]);
        }
        Ok(vec![true; blocks.len()])
    }
}
#[test]
fn observations_during_execution_keep_their_new_batch_and_fingerprint() {
    let backend = Arc::new(ObservingBackend {
        calls: Mutex::new(Vec::new()),
    });
    let service = manual_service(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.observe_main(8, [3; 16]);
    expire_batch(&db);
    let before = Instant::now();
    assert!(run_batch(&service, &db));
    {
        let work = lock(&db.work);
        let due = work.batch_due.unwrap();
        assert!(due >= before + BATCH_DELAY);
        assert_eq!(work.pages[&8].observation, [4; 16]);
        assert!(
            work.pages
                .values()
                .all(|page| page.state == PageState::Pending && page.due == due)
        );
    }
    assert_eq!(db.snapshot().committed_replacements, 0);
    expire_batch(&db);
    assert!(run_batch(&service, &db));
    assert_eq!(*lock(&backend.calls), vec![vec![8], vec![8, 9]]);
    assert_eq!(db.snapshot().committed_replacements, 2);
    service.shutdown().unwrap();
}

#[test]
fn committed_replacement_does_not_claim_physical_retirement() {
    let backend = Backend::new(Vec::new());
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.observe_main(41, [3; 16]);
    until(|| db.snapshot().committed_replacements == 1);
    db.observe_main(41, [3; 16]);
    assert_eq!(db.snapshot().pending_pages, 0);
    assert_eq!(db.snapshot().main_damage, 1);
    assert_eq!(*lock(&backend.calls), vec![vec![41]]);
    db.observe_main(41, [4; 16]);
    until(|| lock(&backend.calls).len() == 2 && db.snapshot().committed_replacements == 1);
    db.retire_main(41);
    assert_eq!(db.snapshot().main_damage, 0);
    db.application_close(true);
    service.shutdown().unwrap();
}

#[test]
fn pages_removed_after_discovery_need_no_replacement() {
    let backend = Backend::new(vec![Ok(vec![false])]);
    let service = Service::with_backend(backend);
    let db = database(&service);
    db.application_open(true);
    db.observe_main(100, [3; 16]);
    until(|| db.snapshot().main_damage == 0);
    assert_eq!(db.snapshot().committed_replacements, 0);
    service.shutdown().unwrap();
}

#[test]
fn busy_work_remains_queued_while_prefix_checks_can_finish() {
    let backend = Backend::new(vec![
        Err(Error::new(ffi::SQLITE_BUSY, "writer busy")),
        Ok(vec![true]),
    ]);
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    let checked = Arc::new(AtomicBool::new(false));
    let seen = checked.clone();
    let token = db.register_prefix(
        [2; 16],
        Arc::new(move || {
            seen.store(true, Ordering::Release);
            Ok(true)
        }),
    );
    db.application_open(true);
    db.observe_main(10, [3; 16]);
    until(|| db.snapshot().blocked);
    assert_eq!(db.snapshot().pending_pages, 1);
    db.observe_prefix([2; 16]);
    until(|| checked.load(Ordering::Acquire));
    until(|| db.snapshot().committed_replacements == 1);
    assert!(!db.snapshot().blocked);
    db.unregister_prefix(token);
    service.shutdown().unwrap();
}

#[test]
fn failed_repair_keeps_damage_and_diagnostic_without_busy_loop() {
    let backend = Backend::new(vec![Err(Error::corrupt("current page is unreadable"))]);
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.observe_main(8, [3; 16]);
    until(|| db.snapshot().last_error.is_some());
    let status = db.snapshot();
    assert_eq!(status.main_damage, 1);
    assert_eq!(status.committed_replacements, 0);
    assert_eq!(status.pending_pages, 0);
    db.observe_main(8, [3; 16]);
    assert_eq!(lock(&backend.calls).len(), 1);
    service.shutdown().unwrap();
}

#[test]
fn transient_failures_retry_with_backoff_then_stop() {
    let failures = (0..RETRY_LIMIT as usize)
        .map(|_| Err(Error::new(ffi::SQLITE_CANTOPEN, "descriptor limit")))
        .collect();
    let backend = Backend::new(failures);
    let service = manual_service(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.observe_main(8, [3; 16]);
    expire_batch(&db);
    for attempt in 1..=RETRY_LIMIT {
        let before = Instant::now();
        assert!(run_batch(&service, &db));
        let after = Instant::now();
        assert_eq!(lock(&backend.calls).len(), attempt as usize);
        assert!(db.snapshot().last_error.is_some());
        let mut work = lock(&db.work);
        let page = work.pages.get_mut(&8).unwrap();
        assert_eq!(page.attempts, attempt);
        assert!(page.due >= before + RETRY_DELAY * (1 << attempt));
        assert!(page.due <= after + RETRY_DELAY * (1 << attempt));
        assert!(
            page.state
                == if attempt < RETRY_LIMIT {
                    PageState::Pending
                } else {
                    PageState::Failed
                }
        );
        page.due = Instant::now() - Duration::from_secs(1);
    }
    assert!(!run_batch(&service, &db));
    assert_eq!(db.snapshot().pending_pages, 0);
    service.shutdown().unwrap();
}

#[test]
fn wal_observations_never_schedule_sql_and_retire_only_matching_slots() {
    let backend = Backend::new(Vec::new());
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.observe_wal([2; 16], 0);
    db.observe_wal([2; 16], 9);
    db.observe_wal([3; 16], 9);
    db.observe_wal([2; 16], 9);
    assert_eq!(db.snapshot().wal_damage, 3);
    db.retire_wal([2; 16], 5);
    assert_eq!(db.snapshot().wal_damage, 2);
    db.retire_wal_block([3; 16], 9);
    assert_eq!(db.snapshot().wal_damage, 1);
    assert!(lock(&backend.calls).is_empty());
    service.shutdown().unwrap();
}

#[test]
fn observations_are_bounded_and_overflow_is_visible() {
    let service = Service::with_backend(Backend::new(Vec::new()));
    let db = database(&service); // No application handle: jobs cannot run yet.
    for block in 0..QUEUE_LIMIT as u64 + 20 {
        db.observe_main(block, [3; 16]);
        db.observe_wal([2; 16], block);
    }
    let status = db.snapshot();
    assert_eq!(status.pending_pages, QUEUE_LIMIT);
    assert_eq!(status.wal_damage, QUEUE_LIMIT);
    assert!(status.queue_full);
    db.retire_main_from(10);
    assert!(db.snapshot().queue_full);
    db.retire_wal([2; 16], 10);
    assert!(!db.snapshot().queue_full);
    service.shutdown().unwrap();
}

#[test]
fn last_application_close_cancels_pending_work_without_flushing() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let backend = Arc::new(WaitingBackend {
        entered: entered_tx,
        release: Mutex::new(release_rx),
        cancelled: AtomicBool::new(false),
    });
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.application_open(true);
    db.observe_main(42, [3; 16]);
    entered_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    db.observe_main(43, [3; 16]);
    db.application_close(true);
    assert_eq!(db.snapshot().applications, 1);
    assert_eq!(db.snapshot().pending_pages, 2);
    db.application_close(true);
    assert_eq!(db.snapshot().applications, 0);
    assert_eq!(db.snapshot().pending_pages, 0);
    release_tx.send(()).unwrap();
    until(|| !db.snapshot().active);
    assert!(backend.cancelled.load(Ordering::Acquire));
    assert_eq!(db.snapshot().committed_replacements, 0);
    service.shutdown().unwrap();
}

#[test]
fn last_writable_close_cancels_work_even_with_readers_open() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let backend = Arc::new(WaitingBackend {
        entered: entered_tx,
        release: Mutex::new(release_rx),
        cancelled: AtomicBool::new(false),
    });
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    db.application_open(false);
    db.application_open(true);
    db.application_open(true);
    db.observe_main(42, [3; 16]);
    entered_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    for block in 0..QUEUE_LIMIT as u64 + 1 {
        db.observe_main(block, [3; 16]);
    }
    assert!(db.snapshot().queue_full);
    db.application_close(true);
    assert_eq!(db.snapshot().applications, 2);
    assert_eq!(db.snapshot().pending_pages, QUEUE_LIMIT);
    db.application_close(true);
    assert_eq!(db.snapshot().applications, 1);
    assert_eq!(db.snapshot().pending_pages, 0);
    assert!(!db.snapshot().queue_full);
    release_tx.send(()).unwrap();
    until(|| !db.snapshot().active);
    assert!(backend.cancelled.load(Ordering::Acquire));
    assert_eq!(db.snapshot().committed_replacements, 0);
    // The surviving reader can still report passive observations.
    db.observe_zero_slots(1);
    assert_eq!(db.snapshot().zero_slot_reads, 1);
    db.application_close(false);
    assert_eq!(db.snapshot().applications, 0);
    assert_eq!(db.cancellation.load(Ordering::Acquire), 1);
    service.shutdown().unwrap();
}

#[test]
fn read_only_close_does_not_cancel_a_writable_lifetime() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let backend = Arc::new(WaitingBackend {
        entered: entered_tx,
        release: Mutex::new(release_rx),
        cancelled: AtomicBool::new(false),
    });
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.application_open(false);
    db.observe_main(42, [3; 16]);
    entered_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    db.application_close(false);
    assert_eq!(db.snapshot().applications, 1);
    assert_eq!(db.snapshot().pending_pages, 1);
    assert_eq!(db.cancellation.load(Ordering::Acquire), 0);
    release_tx.send(()).unwrap();
    until(|| db.snapshot().committed_replacements == 1);
    assert!(!backend.cancelled.load(Ordering::Acquire));
    db.application_close(true);
    service.shutdown().unwrap();
}

struct WaitingBackend {
    entered: mpsc::Sender<()>,
    release: Mutex<mpsc::Receiver<()>>,
    cancelled: AtomicBool,
}
impl RepairBackend for WaitingBackend {
    fn rewrite(
        &self,
        _database: &Database,
        blocks: &[u64],
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<bool>> {
        self.entered.send(()).unwrap();
        lock(&self.release)
            .recv_timeout(Duration::from_secs(3))
            .unwrap();
        let cancelled = cancelled();
        self.cancelled.store(cancelled, Ordering::Release);
        if cancelled {
            Err(Error::new(ffi::SQLITE_ABORT, "cancelled before commit"))
        } else {
            Ok(vec![true; blocks.len()])
        }
    }
}
#[test]
fn concurrent_shutdown_waits_for_active_worker_and_cancels_before_commit() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let backend = Arc::new(WaitingBackend {
        entered: entered_tx,
        release: Mutex::new(release_rx),
        cancelled: AtomicBool::new(false),
    });
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.observe_main(42, [3; 16]);
    entered_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    let (closed_tx, closed_rx) = mpsc::channel();
    let first = service.clone();
    let first_closed = closed_tx.clone();
    let a = thread::spawn(move || {
        first.shutdown().unwrap();
        first_closed.send(()).unwrap();
    });
    let second = service.clone();
    let b = thread::spawn(move || {
        second.shutdown().unwrap();
        closed_tx.send(()).unwrap();
    });
    until(|| db.snapshot().stopped);
    assert!(closed_rx.try_recv().is_err());
    release_tx.send(()).unwrap();
    closed_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    closed_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    a.join().unwrap();
    b.join().unwrap();
    assert!(backend.cancelled.load(Ordering::Acquire));
    assert_eq!(db.snapshot().committed_replacements, 0);
    service.shutdown().unwrap();
}

#[test]
fn same_database_reuses_work_but_copied_identity_at_another_path_does_not() {
    let service = Service::with_backend(Backend::new(Vec::new()));
    let db = database(&service);
    assert!(Arc::ptr_eq(&db, &database(&service)));
    let other = service.register_database(CString::new("/a/copy").unwrap(), [1; 16]);
    assert!(!Arc::ptr_eq(&db, &other));
    service.shutdown().unwrap();
}

#[test]
fn status_is_read_only_and_encodes_diagnostic_strings() {
    let service = Service::with_backend(Backend::new(Vec::new()));
    let db = database(&service);
    lock(&db.work).last_error = Some(Error::new(10, "line\n\"quote\"\\\u{1}"));
    let status = db.snapshot().json();
    assert!(status.contains(r#""message":"line\n\"quote\"\\\u0001""#));
    assert!(status.contains(r#""pending_pages":0"#));
    assert!(!status.contains("available"));
    assert!(!is_worker());
    service.shutdown().unwrap();
}

#[test]
fn unregistered_prefix_does_not_receive_future_checks() {
    let service = Service::with_backend(Backend::new(Vec::new()));
    let db = database(&service);
    let calls = Arc::new(AtomicUsize::new(0));
    let observed = calls.clone();
    let token = db.register_prefix(
        [2; 16],
        Arc::new(move || {
            observed.fetch_add(1, Ordering::Relaxed);
            Ok(true)
        }),
    );
    db.unregister_prefix(token);
    db.observe_prefix([2; 16]);
    db.application_open(true);
    assert_eq!(db.snapshot().prefix_pending, 0);
    assert_eq!(calls.load(Ordering::Relaxed), 0);
    service.shutdown().unwrap();
}

#[test]
fn poisoned_work_stops_the_worker_without_attempting_sql() {
    let backend = Backend::new(Vec::new());
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    db.application_open(true);
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let _work = db.work.lock().unwrap();
        panic!("injected damaged work state");
    }));
    service.wake();
    until(|| lock(&service.state).finished);
    assert!(lock(&backend.calls).is_empty());
    assert!(service.shutdown().is_err());
}

#[test]
fn poisoned_scheduler_can_still_stop_and_join() {
    let service = Service::with_backend(Backend::new(Vec::new()));
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let _state = service.state.lock().unwrap();
        panic!("injected damaged scheduler state");
    }));
    assert!(service.shutdown().is_err());
    assert!(service.shutdown().is_err());
}

struct CleanupFailure;
impl RepairBackend for CleanupFailure {
    fn cleanup_error(&self) -> Option<Error> {
        Some(Error::new(
            ffi::SQLITE_BUSY,
            "owned connection did not close",
        ))
    }
    fn rewrite(
        &self,
        _database: &Database,
        blocks: &[u64],
        _cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<bool>> {
        Ok(vec![true; blocks.len()])
    }
}
#[test]
fn owned_connection_cleanup_failure_prevents_successful_shutdown() {
    let service = Service::with_backend(Arc::new(CleanupFailure));
    let db = database(&service);
    db.application_open(true);
    until(|| db.snapshot().stopped);
    assert_eq!(
        service.shutdown().unwrap_err().message,
        "owned connection did not close"
    );
    assert_eq!(service.shutdown().unwrap_err().code, ffi::SQLITE_BUSY);
}

#[test]
fn application_lifetime_changes_wait_for_work_ownership() {
    let service = Service::with_backend(Backend::new(Vec::new()));
    let db = database(&service);
    db.application_open(true);
    let work = lock(&db.work);
    let (started_tx, started_rx) = mpsc::channel();
    let (finished_tx, finished_rx) = mpsc::channel();
    let closing = db.clone();
    let handle = thread::spawn(move || {
        started_tx.send(()).unwrap();
        closing.application_close(true);
        finished_tx.send(()).unwrap();
    });
    started_rx.recv().unwrap();
    assert!(finished_rx.recv_timeout(Duration::from_millis(30)).is_err());
    // The count cannot reach zero before cancellation owns the queued work.
    assert_eq!(db.applications.load(Ordering::Acquire), 1);
    assert_eq!(db.cancellation.load(Ordering::Acquire), 0);
    drop(work);
    finished_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    handle.join().unwrap();
    assert_eq!(db.applications.load(Ordering::Acquire), 0);
    assert_eq!(db.cancellation.load(Ordering::Acquire), 1);
    service.shutdown().unwrap();
}

struct ReopenedBackend {
    calls: AtomicUsize,
    entered: mpsc::Sender<()>,
    release: Mutex<mpsc::Receiver<()>>,
}
impl RepairBackend for ReopenedBackend {
    fn rewrite(
        &self,
        _database: &Database,
        blocks: &[u64],
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<bool>> {
        assert_eq!(blocks, &[42]);
        if self.calls.fetch_add(1, Ordering::AcqRel) == 0 {
            self.entered.send(()).unwrap();
            lock(&self.release)
                .recv_timeout(Duration::from_secs(3))
                .unwrap();
            assert!(cancelled());
            Err(Error::new(ffi::SQLITE_ABORT, "old application closed"))
        } else {
            assert!(!cancelled());
            Ok(vec![true])
        }
    }
}
#[test]
fn cancelled_old_batch_preserves_same_page_queued_after_reopen() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let backend = Arc::new(ReopenedBackend {
        calls: AtomicUsize::new(0),
        entered: entered_tx,
        release: Mutex::new(release_rx),
    });
    let service = Service::with_backend(backend.clone());
    let db = database(&service);
    db.application_open(true);
    db.observe_main(42, [3; 16]);
    entered_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    db.application_close(true);
    db.application_open(true);
    // Reuse the fingerprint deliberately: cancellation must distinguish lifetimes.
    db.observe_main(42, [3; 16]);
    release_tx.send(()).unwrap();
    until(|| db.snapshot().committed_replacements == 1);
    assert_eq!(backend.calls.load(Ordering::Acquire), 2);
    assert_eq!(db.snapshot().pending_pages, 0);
    assert_eq!(db.snapshot().main_damage, 1);
    service.shutdown().unwrap();
}

#[test]
fn main_truncation_retires_removed_committed_damage() {
    let service = Service::with_backend(Backend::new(Vec::new()));
    let db = database(&service);
    db.application_open(true);
    for block in [0, 41, 42] {
        db.observe_main(block, [3; 16]);
    }
    until(|| db.snapshot().committed_replacements == 3);
    db.retire_main_from(41);
    assert_eq!(db.snapshot().main_damage, 1);
    assert_eq!(db.snapshot().committed_replacements, 1);
    assert!(lock(&db.work).pages.contains_key(&0));
    db.retire_main_from(0);
    assert_eq!(db.snapshot().main_damage, 0);
    service.shutdown().unwrap();
}

#[test]
fn cutoffs_refusals_and_zero_reads_remain_visible_in_status() {
    let service = Service::with_backend(Backend::new(Vec::new()));
    let db = database(&service);
    db.record_cutoff(40960, 8192, 2);
    db.record_cutoff(16384, 4096, 0);
    db.record_refusal();
    db.observe_zero_slots(3);
    db.record_main_sync_failure();
    let status = db.snapshot();
    assert_eq!(status.wal_cutoffs, 2);
    assert_eq!(status.wal_cutoff_bytes, 12288);
    assert_eq!(status.wal_cutoff_commits, 2);
    assert_eq!(status.wal_last_cutoff, Some(16384));
    assert_eq!(status.wal_refusals, 1);
    assert_eq!(status.zero_slot_reads, 3);
    assert_eq!(status.main_sync_failures, 1);
    let json = status.json();
    assert!(json.contains("\"wal_cutoff_commits\":2"));
    assert!(json.contains("\"wal_last_cutoff\":16384"));
    assert!(json.contains("\"zero_slot_reads\":3"));
    service.shutdown().unwrap();
}

#[test]
fn prefix_checks_skip_unavailable_handles_but_stop_after_success_or_error() {
    for fails in [false, true] {
        let service = manual_service(Backend::new(Vec::new()));
        let db = database(&service);
        db.application_open(true);
        let calls = Arc::new(Mutex::new(Vec::new()));
        for handle in 0..3 {
            let calls = calls.clone();
            db.register_prefix(
                [2; 16],
                Arc::new(move || {
                    lock(&calls).push(handle);
                    match handle {
                        0 => Ok(false),
                        1 if fails => Err(Error::new(ffi::SQLITE_IOERR, "prefix sync failed")),
                        1 => Ok(true),
                        _ => panic!("prefix check continued after an actual result"),
                    }
                }),
            );
        }
        db.observe_prefix([2; 16]);
        assert!(service.check_prefix(&db));
        assert_eq!(*lock(&calls), vec![0, 1]);
        let status = db.snapshot();
        assert_eq!(status.prefix_degraded, usize::from(fails));
        assert_eq!(status.prefix_pending, 0);
        assert_eq!(status.last_error.is_some(), fails);
        if fails {
            assert_eq!(status.last_error.unwrap().message, "prefix sync failed");
        }
        assert!(!service.check_prefix(&db));
        service.shutdown().unwrap();
    }
}

#[test]
fn unavailable_prefix_handles_do_not_claim_repair_or_clear_diagnostics() {
    let service = manual_service(Backend::new(Vec::new()));
    let db = database(&service);
    db.application_open(true);
    db.register_prefix([2; 16], Arc::new(|| Ok(false)));
    db.register_prefix([2; 16], Arc::new(|| Ok(false)));
    db.observe_prefix([2; 16]);
    lock(&db.work).last_error = Some(Error::corrupt("earlier prefix damage"));
    assert!(service.check_prefix(&db));
    let status = db.snapshot();
    assert_eq!(status.prefix_degraded, 1);
    assert_eq!(status.prefix_pending, 0);
    assert_eq!(status.last_error.unwrap().message, "earlier prefix damage");
    assert!(!service.check_prefix(&db));
    service.shutdown().unwrap();
}

#[test]
fn prefix_result_cannot_clear_a_new_observation() {
    let service = manual_service(Backend::new(Vec::new()));
    let db = database(&service);
    db.application_open(true);
    let observed = Arc::downgrade(&db);
    db.register_prefix(
        [2; 16],
        Arc::new(move || {
            observed.upgrade().unwrap().observe_prefix([2; 16]);
            Ok(true)
        }),
    );
    db.observe_prefix([2; 16]);
    assert!(service.check_prefix(&db));
    assert_eq!(db.snapshot().prefix_degraded, 1);
    assert_eq!(db.snapshot().prefix_pending, 1);
    service.shutdown().unwrap();
}

#[test]
fn prefix_fallback_stops_when_the_writer_closes_or_the_service_stops() {
    for stop_service in [false, true] {
        let service = manual_service(Backend::new(Vec::new()));
        let db = database(&service);
        db.application_open(true);
        let observed = Arc::downgrade(&db);
        let running = Arc::downgrade(&service);
        db.register_prefix(
            [2; 16],
            Arc::new(move || {
                if stop_service {
                    lock(&running.upgrade().unwrap().state).stopped = true;
                } else {
                    observed.upgrade().unwrap().application_close(true);
                }
                Ok(false)
            }),
        );
        db.register_prefix([2; 16], Arc::new(|| panic!("check after cancellation")));
        db.observe_prefix([2; 16]);
        assert!(service.check_prefix(&db));
        assert_eq!(db.snapshot().prefix_degraded, 1);
        service.shutdown().unwrap();
    }
}

#[test]
fn operation_admission_requires_an_application_and_reopens_without_restart() {
    let service = manual_service(Backend::new(Vec::new()));
    let db = database(&service);
    assert!(service.operation().is_none());
    db.application_open(true);
    let operation = service.operation().unwrap();
    let ticket = db.application_close(true);
    assert!(ticket.is_some());
    drop(operation);
    service.wait_for_idle(ticket);
    assert!(service.operation().is_none());
    assert!(!lock(&service.state).stopped);
    db.application_open(true);
    assert!(service.operation().is_some());
    service.wait_for_idle(db.application_close(true));
    service.shutdown().unwrap();
}

#[test]
fn close_ticket_does_not_wait_for_work_started_by_a_later_open() {
    let service = manual_service(Backend::new(Vec::new()));
    let db = database(&service);
    db.application_open(true);
    let old = service.operation().unwrap();
    let ticket = db.application_close(true);
    db.application_open(true); // Opening must not wait on the old operation.
    drop(old);
    let new = service.operation().unwrap();
    let (sent, received) = mpsc::channel();
    let closing = service.clone();
    let waiter = thread::spawn(move || {
        closing.wait_for_idle(ticket);
        sent.send(()).unwrap();
    });
    received.recv_timeout(Duration::from_secs(3)).unwrap();
    waiter.join().unwrap();
    assert!(lock(&service.state).active);
    drop(new);
    service.wait_for_idle(db.application_close(true));
    service.shutdown().unwrap();
}

#[test]
fn final_close_waits_for_prefix_io_as_well_as_sql() {
    let service = Service::with_backend(Backend::new(Vec::new()));
    let db = database(&service);
    let (entered_tx, entered_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let release = Mutex::new(release_rx);
    db.register_prefix(
        [2; 16],
        Arc::new(move || {
            entered_tx.send(()).unwrap();
            lock(&release).recv_timeout(Duration::from_secs(3)).unwrap();
            Ok(true)
        }),
    );
    db.application_open(true);
    db.observe_prefix([2; 16]);
    entered_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    let ticket = db.application_close(true);
    let (finished_tx, finished_rx) = mpsc::channel();
    let closing = service.clone();
    let handle = thread::spawn(move || {
        closing.wait_for_idle(ticket);
        finished_tx.send(()).unwrap();
    });
    assert!(finished_rx.recv_timeout(Duration::from_millis(30)).is_err());
    release_tx.send(()).unwrap();
    finished_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    handle.join().unwrap();
    assert!(!lock(&service.state).active);
    assert!(service.operation().is_none());
    service.shutdown().unwrap();
}

#[test]
fn poisoned_work_cannot_skip_final_connection_cancellation() {
    let service = manual_service(Backend::new(Vec::new()));
    let db = database(&service);
    db.application_open(true);
    let operation = service.operation().unwrap();
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let _work = db.work.lock().unwrap();
        panic!("damaged work state before close");
    }));
    let ticket = db.application_close(true);
    assert!(ticket.is_some());
    assert_eq!(lock(&service.state).applications, 0);
    assert!(lock(&service.state).stopped);
    drop(operation);
    service.wait_for_idle(ticket);
    assert!(service.operation().is_none());
    assert!(service.shutdown().is_err());
}
