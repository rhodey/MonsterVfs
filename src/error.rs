use std::fmt;

pub const NOMEM: i32 = 7;
pub const IOERR: i32 = 10;
pub const CORRUPT: i32 = 11;
pub const CANTOPEN: i32 = 14;
pub const TOOBIG: i32 = 18;
pub const MISUSE: i32 = 21;
pub const IOERR_DIR_FSYNC: i32 = IOERR | (5 << 8);
pub const IOERR_FSTAT: i32 = IOERR | (7 << 8);
pub const IOERR_LOCK: i32 = IOERR | (15 << 8);
pub const IOERR_DATA: i32 = IOERR | (32 << 8);
pub const IOERR_CORRUPTFS: i32 = IOERR | (33 << 8);
/// WAL damage below the backfill watermark: main may hold later pages, so no
/// retained prefix is consistent. Public as `MONSTERVFS_CORRUPT_BACKFILLED`.
pub const CORRUPT_BACKFILLED: i32 = CORRUPT | (0x4d << 8);
pub const SYNC_NORMAL: i32 = 2;
pub const SYNC_FULL: i32 = 3;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Error {
    pub code: i32,
    pub message: String,
}

impl Error {
    pub fn new(code: i32, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }

    pub fn corrupt(message: impl Into<String>) -> Self {
        Self::new(IOERR_DATA, message)
    }
    /// Bytes the storage refuses to return are as lost as bytes that fail verification.
    pub fn unreadable(&self) -> bool {
        self.code == IOERR_DATA || self.code == IOERR_CORRUPTFS
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} (SQLite {})", self.message, self.code)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

pub(crate) fn zeros<T: Default + Clone>(len: usize) -> Result<Vec<T>> {
    let mut data = Vec::new();
    data.try_reserve_exact(len)
        .map_err(|_| Error::new(NOMEM, "allocation failed"))?;
    data.resize(len, T::default());
    Ok(data)
}
