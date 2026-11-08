use crate::config::{BLOCK_SIZE, Settings};
use crate::error::{Error, Result, TOOBIG};

pub const HEADER_SIZE: usize = 8192;
pub const HEADER_REGION_SIZE: usize = 4096;
pub const HEADER_RECORD_SIZE: usize = if cfg!(feature = "copy") {
    HEADER_REGION_SIZE
} else {
    256
};
pub const HEADER_DATA_SIZE: usize = 128;
pub const METADATA_SIZE: usize = 32;
pub const CONTROL_SIZE: usize = 8192;
pub const CONTROL_REGION_SIZE: usize = 4096;
pub const WAL_HEADER_SIZE: usize = 32;
pub const WAL_FRAME_HEADER_SIZE: usize = 24;
pub const HASH_SIZE: usize = 16;
pub const HASH_COPIES_SIZE: usize = 3 * HASH_SIZE;
pub const PARITY_OFFSET: usize = METADATA_SIZE + 2 * HASH_SIZE;
/// Taken from the end of every SQLite page, which keeps the slot's trailer there.
pub const RESERVED_SIZE: usize = 48;
pub const CONTROL_MAGIC: [u8; 8] = *b"MVFSCTRL";

#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
#[repr(u32)]
pub enum Kind {
    Main = 1,
    Wal = 2,
    Temporary = 3,
}

impl TryFrom<u32> for Kind {
    type Error = Error;

    fn try_from(value: u32) -> Result<Self> {
        match value {
            1 => Ok(Self::Main),
            2 => Ok(Self::Wal),
            3 => Ok(Self::Temporary),
            _ => Err(Error::corrupt("file kind is invalid")),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Profile {
    pub settings: Settings,
    pub kind: Kind,
    pub id: [u8; 16],
}

impl Kind {
    pub fn content_start(self) -> u64 {
        if self == Kind::Wal {
            (HEADER_SIZE + CONTROL_SIZE) as u64
        } else {
            HEADER_SIZE as u64
        }
    }
}
impl Profile {
    pub fn new(settings: Settings, kind: Kind, id: [u8; 16]) -> Result<Self> {
        settings.validate()?;
        Ok(Self { settings, kind, id })
    }

    pub fn parity_bytes(&self) -> usize {
        if self.kind == Kind::Temporary {
            0
        } else {
            self.settings.parity_bytes
        }
    }

    pub fn content_start(&self) -> u64 {
        self.kind.content_start()
    }

    pub fn logical_base(&self) -> u64 {
        if self.kind == Kind::Wal {
            WAL_HEADER_SIZE as u64
        } else {
            0
        }
    }

    pub fn frame_header_size(&self) -> usize {
        if self.kind == Kind::Wal {
            WAL_FRAME_HEADER_SIZE
        } else {
            0
        }
    }

    pub fn payload_size(&self) -> usize {
        BLOCK_SIZE + self.frame_header_size()
    }

    pub fn reserved_size(&self) -> usize {
        if self.kind == Kind::Temporary {
            0
        } else {
            RESERVED_SIZE
        }
    }

    /// Whether slots hold their page twice. Temporary files are the same in both builds.
    fn copied(&self) -> bool {
        cfg!(feature = "copy") && self.kind != Kind::Temporary
    }

    pub fn slot_size(&self) -> usize {
        // Every allowed parity size, WAL header, and temporary layout fits two blocks.
        2 * BLOCK_SIZE
    }

    pub fn prefix_size(&self) -> usize {
        self.slot_size() - self.payload_size()
    }

    /// Where the part of a slot starts that can prove itself with the rest unreadable.
    pub fn standalone_start(&self) -> usize {
        if self.copied() {
            0
        } else {
            self.slot_size() - BLOCK_SIZE
        }
    }

    /// The physical bytes of a slot holding `present` payload bytes. Copied slots are
    /// always whole, because a trailer is all that can prove them: only a crash leaves
    /// less, and that tail counts as absent. SQLite never reads or depends on less.
    pub fn stored_length(&self, present: usize) -> usize {
        if self.copied() {
            self.slot_size()
        } else {
            self.prefix_size() + present
        }
    }

    /// Where a slot keeps each copy of its data hash and of its parity hash. Damage comes
    /// in runs, so the pairs sit apart: at the start, middle and end of the prefix.
    pub fn hash_offsets(&self) -> ([usize; 3], [usize; 3]) {
        let first = METADATA_SIZE;
        if self.kind == Kind::Temporary {
            return ([first, first + HASH_SIZE, first + 2 * HASH_SIZE], [0; 3]);
        }
        let pair = 2 * HASH_SIZE;
        let last = self.prefix_size() - pair;
        let parity_end = PARITY_OFFSET + self.parity_bytes();
        let middle = parity_end + (last - parity_end - pair) / 2;
        let data = [first, middle, last];
        (data, data.map(|at| at + HASH_SIZE))
    }

    pub fn max_blocks(&self) -> u64 {
        (i64::MAX as u64 - self.content_start()) / self.slot_size() as u64
    }

    pub fn max_logical_length(&self) -> u64 {
        self.max_blocks() * self.payload_size() as u64 + self.logical_base()
    }

    pub fn slot_offset(&self, block: u64) -> Result<u64> {
        if block > self.max_blocks() {
            return Err(Error::new(TOOBIG, "block offset exceeds file limit"));
        }
        block
            .checked_mul(self.slot_size() as u64)
            .and_then(|offset| offset.checked_add(self.content_start()))
            .ok_or_else(|| Error::new(TOOBIG, "physical offset overflow"))
    }

    pub fn block_offset(&self, block: u64) -> Result<u64> {
        if block >= self.max_blocks() {
            return Err(Error::new(TOOBIG, "block exceeds file limit"));
        }
        block
            .checked_mul(self.payload_size() as u64)
            .ok_or_else(|| Error::new(TOOBIG, "logical offset overflow"))
    }

    /// Where a truncate to `length` ends. SQLite cannot use part of a WAL frame, so a frame
    /// cut in two is gone on any VFS. This removes it whole, without reading it: the only such
    /// cut is a size limit landing past the last commit, and a dead frame must not stop a writer.
    pub fn truncated_length(&self, length: u64) -> u64 {
        if self.kind != Kind::Wal {
            return length;
        }
        length - length.saturating_sub(self.logical_base()) % self.payload_size() as u64
    }

    pub fn physical_length(&self, logical: u64) -> Result<u64> {
        if logical > self.max_logical_length() {
            return Err(Error::new(TOOBIG, "logical length exceeds file limit"));
        }
        let payload = self.payload_size() as u64;
        let data = logical.saturating_sub(self.logical_base());
        let whole = self.slot_offset(data / payload)?;
        let tail = data % payload;
        Ok(whole
            + if tail == 0 {
                0
            } else {
                self.stored_length(tail as usize) as u64
            })
    }

    pub fn logical_length(&self, physical: u64) -> Result<u64> {
        if physical < HEADER_SIZE as u64 || physical > i64::MAX as u64 {
            return Err(Error::corrupt("physical file length is invalid"));
        }
        let contents = physical.saturating_sub(self.content_start());
        let size = self.slot_size() as u64;
        let blocks = contents / size;
        let tail = contents % size;
        if blocks > self.max_blocks()
            || (blocks == self.max_blocks() && tail > self.prefix_size() as u64)
        {
            return Err(Error::corrupt("physical file length exceeds limit"));
        }
        let partial = if self.copied() {
            0
        } else {
            tail.saturating_sub(self.prefix_size() as u64)
        };
        let data = blocks
            .checked_mul(self.payload_size() as u64)
            .and_then(|offset| offset.checked_add(partial))
            .ok_or_else(|| Error::corrupt("logical file length overflow"))?;
        Ok(if data == 0 {
            0
        } else {
            data + self.logical_base()
        })
    }
}

#[cfg(test)]
#[path = "../tests/unit/format.rs"]
mod tests;
