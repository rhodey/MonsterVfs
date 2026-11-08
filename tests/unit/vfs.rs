use super::*;

#[test]
fn recovery_unwind_releases_and_keeps_the_original_error() {
    let mut failure = None;
    let original = Error::new(SQLITE_IOERR_FSYNC, "original sync error");
    let mut released = false;
    let result = unwind_recovery(&mut failure, original.clone(), || {
        released = true;
        Ok(())
    });
    assert_eq!(result, original);
    assert!(released);
    assert!(failure.is_none());
}

#[test]
fn a_failed_release_stops_the_handle_without_hiding_the_cause() {
    let mut failure = None;
    let original = Error::corrupt("original WAL error");
    let result = unwind_recovery(&mut failure, original.clone(), || {
        Err(Error::new(SQLITE_IOERR_SHMLOCK, "release failed"))
    });
    assert_eq!(result, original);
    assert_eq!(failure.unwrap().message, "release failed");
}

#[test]
fn the_first_failure_is_preserved() {
    let earlier = Error::new(SQLITE_IOERR_WRITE, "previous failure");
    let mut failure = Some(earlier.clone());
    unwind_recovery(&mut failure, Error::corrupt("later"), || {
        Err(Error::new(SQLITE_IOERR_SHMLOCK, "release failed"))
    });
    assert_eq!(failure, Some(earlier));
}

#[test]
fn callback_panics_stop_work_and_release_the_native_handle() {
    use std::sync::atomic::{AtomicI32, AtomicUsize};

    struct Calls {
        methods: sqlite3_io_methods,
        writes: AtomicUsize,
        syncs: AtomicUsize,
        closes: AtomicUsize,
        close_result: AtomicI32,
    }
    #[repr(C)]
    struct File {
        base: sqlite3_file,
        calls: *const Calls,
        fd: i32,
    }
    unsafe extern "C" fn native_open(
        vfs: *mut sqlite3_vfs,
        _name: *const c_char,
        file: *mut sqlite3_file,
        flags: i32,
        actual: *mut i32,
    ) -> i32 {
        let fd = unsafe { libc::open(c"/dev/null".as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
        if fd < 0 {
            return SQLITE_CANTOPEN;
        }
        unsafe {
            let calls = (*vfs).pAppData.cast::<Calls>();
            file.cast::<File>().write(File {
                base: sqlite3_file {
                    pMethods: &(*calls).methods,
                },
                calls,
                fd,
            });
            *actual = flags;
        }
        SQLITE_OK
    }
    unsafe extern "C" fn native_write(
        file: *mut sqlite3_file,
        _bytes: *const c_void,
        _n: i32,
        _offset: i64,
    ) -> i32 {
        unsafe { &*(*file.cast::<File>()).calls }
            .writes
            .fetch_add(1, Ordering::Relaxed);
        SQLITE_OK
    }
    unsafe extern "C" fn native_sync(file: *mut sqlite3_file, _flags: i32) -> i32 {
        unsafe { &*(*file.cast::<File>()).calls }
            .syncs
            .fetch_add(1, Ordering::Relaxed);
        SQLITE_OK
    }
    unsafe extern "C" fn native_close(file: *mut sqlite3_file) -> i32 {
        let file = unsafe { &*file.cast::<File>() };
        let calls = unsafe { &*file.calls };
        calls.closes.fetch_add(1, Ordering::Relaxed);
        let result = unsafe { libc::close(file.fd) };
        calls.close_result.store(result, Ordering::Relaxed);
        if result == 0 {
            SQLITE_OK
        } else {
            SQLITE_IOERR_CLOSE
        }
    }

    // Both an ordinary unwind and one that poisons Io must be contained.
    for holding_lock in [false, true] {
        let calls = Box::new(Calls {
            methods: sqlite3_io_methods {
                iVersion: 1,
                xClose: Some(native_close),
                xWrite: Some(native_write),
                xSync: Some(native_sync),
                ..unsafe { std::mem::zeroed() }
            },
            writes: AtomicUsize::new(0),
            syncs: AtomicUsize::new(0),
            closes: AtomicUsize::new(0),
            close_result: AtomicI32::new(-1),
        });
        let mut vfs = sqlite3_vfs {
            szOsFile: std::mem::size_of::<File>() as i32,
            pAppData: (&*calls as *const Calls).cast_mut().cast(),
            xOpen: Some(native_open),
            ..unsafe { std::mem::zeroed() }
        };
        let (native, _) =
            unsafe { Native::open(&mut vfs, ptr::null(), None, SQLITE_OPEN_READWRITE, false, 0) }
                .unwrap();
        let io = Arc::new(Mutex::new(Io {
            native,
            mapped: None,
            kind: None,
            read_only: false,
            limited: None,
            failure: None,
            closed: false,
            shm_mapped: false,
        }));
        let weak = Arc::downgrade(&io);
        let mut wrapper = Wrapper {
            base: sqlite3_file { pMethods: &METHODS },
            state: Box::into_raw(Box::new(State {
                io,
                context: None,
                database: None,
                repairs: false,
                prefix_token: None,
            })),
        };
        let file = &mut wrapper.base as *mut sqlite3_file;
        let byte = 1u8;
        assert_eq!(
            unsafe { write(file, (&byte as *const u8).cast(), 1, 0) },
            SQLITE_OK
        );
        assert_eq!(calls.writes.load(Ordering::Relaxed), 1);
        assert_eq!(
            invoke(file, |s| {
                let _held = holding_lock.then(|| lock(&s.io).unwrap());
                panic!("injected callback panic");
            }),
            SQLITE_IOERR
        );
        {
            let io = weak.upgrade().unwrap();
            let io = io.lock().unwrap_or_else(|e| e.into_inner());
            assert_eq!(io.failure.as_ref().unwrap().code, SQLITE_IOERR);
            assert_eq!(
                io.failure.as_ref().unwrap().message,
                "VFS callback panicked"
            );
        }
        assert_eq!(
            unsafe { write(file, (&byte as *const u8).cast(), 1, 0) },
            SQLITE_IOERR
        );
        assert_eq!(unsafe { sync(file, SQLITE_SYNC_FULL) }, SQLITE_IOERR);
        assert_eq!(calls.syncs.load(Ordering::Relaxed), 0);
        assert_eq!(calls.writes.load(Ordering::Relaxed), 1);
        // A poisoned mutex reports the failure, but dropping State still closes Native.
        let expected = if holding_lock {
            SQLITE_IOERR
        } else {
            SQLITE_OK
        };
        assert_eq!(unsafe { close(file) }, expected);
        assert!(wrapper.state.is_null());
        assert!(wrapper.base.pMethods.is_null());
        assert!(weak.upgrade().is_none());
        assert_eq!(calls.closes.load(Ordering::Relaxed), 1);
        assert_eq!(calls.close_result.load(Ordering::Relaxed), 0);
        assert_eq!(unsafe { close(file) }, SQLITE_OK);
        assert_eq!(calls.closes.load(Ordering::Relaxed), 1);
    }
}
