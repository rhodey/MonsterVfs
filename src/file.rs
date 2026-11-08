//! Logical file operations. The caller holds physical-I/O exclusion throughout.
use crate::{
    codec::{self, Control},
    config::BLOCK_SIZE,
    error::{
        CORRUPT, CORRUPT_BACKFILLED, Error, NOMEM, Result, SYNC_FULL, SYNC_NORMAL, TOOBIG, zeros,
    },
    format::{
        CONTROL_REGION_SIZE, HEADER_RECORD_SIZE, HEADER_REGION_SIZE, HEADER_SIZE, Kind, Profile,
        RESERVED_SIZE, WAL_FRAME_HEADER_SIZE,
    },
    native::Physical,
    recovery,
};
use std::ops::Range;

// Reads verify every slot, but one callback queues only a bounded repair batch.
// A later read can rediscover damage beyond this limit.
pub(crate) const MAX_READ_OBSERVATIONS: usize = 1024;

#[derive(Clone, Debug)]
pub struct Observation {
    pub block: u64,
    pub fingerprint: [u8; 16],
}
#[derive(Debug)]
pub struct ReadOutcome {
    pub short: bool,
    pub observations: Vec<Observation>,
    pub zero_slots: u64,
}
pub struct Mapped {
    pub profile: Profile,
    pub prefix_repair: [bool; 2],
    // A frame header waits for its body, so that one frame is one slot write.
    pub pending: Option<(u64, [u8; WAL_FRAME_HEADER_SIZE])>,
    // The last commit SQLite holds when it has no shared wal-index to read: what recovery
    // found, then what this handle wrote, since nothing else can write to that WAL.
    pub last_commit: u64,
}
impl Mapped {
    pub fn create(io: &mut impl Physical, profile: Profile) -> Result<Self> {
        io.write(0, &codec::encode_prefix(&profile)?)?;
        if profile.kind != Kind::Temporary {
            // Nothing may make the file look longer than a durable prefix.
            io.sync(SYNC_NORMAL)?;
        }
        if profile.kind == Kind::Wal {
            io.truncate(profile.content_start())?;
        }
        Ok(Self {
            profile,
            prefix_repair: [false; 2],
            pending: None,
            last_commit: 0,
        })
    }
    /// Creation syncs the prefix before anything else, so a file with no room for
    /// content and no usable prefix holds nothing durable. A crash can leave one, since
    /// a file's size may land before its bytes, and an opener must not stop at it.
    pub fn unfinished(io: &mut impl Physical, kind: Kind) -> Result<bool> {
        let physical = io.size()?;
        if physical == 0 {
            return Ok(true);
        }
        if physical > kind.content_start() {
            return Ok(false);
        }
        let bytes = read_prefix(io, physical)?;
        // A plain SQLite file was never ours to replace, nor was the other build's.
        if bytes.starts_with(b"SQLite format 3\0") || codec::other_build(&bytes) {
            return Ok(false);
        }
        match codec::decode_prefix(&bytes) {
            Ok(_) => Ok(physical < HEADER_SIZE as u64),
            Err(error) if error.unreadable() => Ok(true),
            // Conflicting verified identities are not an interrupted creation.
            Err(error) => Err(error),
        }
    }
    pub fn open(io: &mut impl Physical) -> Result<Self> {
        let physical = io.size()?;
        if physical < HEADER_SIZE as u64 {
            return Err(Error::corrupt("incomplete file prefix"));
        }
        let prefix = codec::decode_prefix(&read_prefix(io, physical)?)?;
        Ok(Self {
            profile: prefix.profile,
            prefix_repair: prefix.repair,
            pending: None,
            last_commit: 0,
        })
    }
    pub fn size(&self, io: &mut impl Physical) -> Result<u64> {
        self.profile.logical_length(io.size()?)
    }
    pub fn slot(
        &self,
        io: &mut impl Physical,
        block: u64,
        physical_size: u64,
    ) -> Result<(codec::Decoded, Option<Observation>)> {
        let start = self.profile.slot_offset(block)?;
        if start >= physical_size {
            return Err(Error::corrupt("required slot is missing"));
        }
        let length = (physical_size - start).min(self.profile.slot_size() as u64) as usize;
        let mut bytes = zeros(length)?;
        match io.read(start, &mut bytes) {
            // The sector the storage refused may be one the slot can do without.
            // Zeros prove nothing: they may be bytes we never read.
            Err(error)
                if error.unreadable()
                    && length == self.profile.slot_size()
                    && self.profile.reserved_size() != 0 =>
            {
                bytes.fill(0);
                let size = BLOCK_SIZE;
                let first = self.profile.standalone_start();
                for (index, part) in bytes[first..].chunks_mut(size).enumerate() {
                    match io.read(start + (first + index * size) as u64, part) {
                        Err(failed) if failed.unreadable() => part.fill(0),
                        result => result?,
                    }
                }
                if bytes.iter().all(|&byte| byte == 0) {
                    return Err(error);
                }
            }
            result => result?,
        }
        let decoded = codec::decode_slot(&self.profile, block, &bytes)?;
        self.validate_page_one(block, &decoded.payload)?;
        let observation = decoded.needs_repair.then(|| Observation {
            block,
            fingerprint: xxhash_rust::xxh3::xxh3_128(&bytes).to_be_bytes(),
        });
        Ok((decoded, observation))
    }
    /// The newest control record that verifies and holds a valid SQLite header.
    /// An interrupted write leaves the previous record intact in the other region.
    pub fn control(&self, io: &mut impl Physical) -> Result<Option<Control>> {
        let physical = io.size()?;
        if physical < self.profile.content_start() {
            return Ok(None);
        }
        let mut newest: Option<Control> = None;
        let mut conflict = false;
        let mut unreadable = None;
        for region in 0..2 {
            let mut record = [0u8; HEADER_RECORD_SIZE];
            match io.read(control_offset(region), &mut record) {
                Err(failed) if failed.unreadable() => {
                    unreadable = Some(failed);
                    continue;
                }
                result => result?,
            }
            for control in codec::decode_controls(&self.profile.id, &record)?
                .into_iter()
                .flatten()
            {
                if recovery::validate_header(&control.wal_header).is_err() {
                    continue;
                }
                match newest.as_ref() {
                    Some(current) if control.sequence < current.sequence => {}
                    Some(current) if control.sequence == current.sequence => {
                        conflict |= control != *current;
                    }
                    _ => {
                        newest = Some(control);
                        conflict = false;
                    }
                }
            }
        }
        if conflict {
            return Err(Error::new(
                CORRUPT,
                "WAL control copies differ at the same sequence",
            ));
        }
        match unreadable {
            Some(failed) if !self.hides_nothing(io, newest.as_ref(), physical) => Err(failed),
            _ => Ok(newest),
        }
    }
    /// Whether a record the storage refused can be left out. It may be newer than `control`,
    /// and then `control` would hide its frames from SQLite, and a checkpoint would write
    /// `control` back as the newest. SQLite syncs a header before writing any frame, so a
    /// first frame that fully verifies with these salts proves `control` is current.
    fn hides_nothing(
        &self,
        io: &mut impl Physical,
        control: Option<&Control>,
        physical: u64,
    ) -> bool {
        if physical <= self.profile.content_start() {
            return true;
        }
        let Some(control) = control else {
            return false;
        };
        self.slot(io, 0, physical).is_ok_and(|(frame, _)| {
            !frame.zero && !frame.rebuilt && frame.payload[8..16] == control.wal_header[16..24]
        })
    }
    fn write_control(&self, io: &mut impl Physical, control: &Control) -> Result<()> {
        if io.size()? < self.profile.content_start() {
            io.truncate(self.profile.content_start())?;
        }
        let offset = control_offset(control.sequence % 2);
        io.write(offset, &codec::encode_control(&self.profile.id, control)?)
    }
    /// Records that frames through `frames` may reach main. The caller makes this
    /// durable before main changes: recovery relies on it to refuse a bad cutoff, so
    /// a checkpoint that cannot record its watermark must not proceed.
    pub fn record_backfill(&self, io: &mut impl Physical, frames: u32) -> Result<()> {
        let mut control = self
            .control(io)?
            .ok_or_else(|| Error::new(CORRUPT_BACKFILLED, "WAL has no usable control record"))?;
        control.sequence += 1;
        control
            .backfill_salts
            .copy_from_slice(&control.wal_header[16..24]);
        control.backfill_frames = frames;
        self.write_control(io, &control)
    }
    pub fn read(
        &self,
        io: &mut impl Physical,
        offset: u64,
        output: &mut [u8],
    ) -> Result<ReadOutcome> {
        output.fill(0);
        let physical = io.size()?;
        let length = self.profile.logical_length(physical)?;
        let available = length.saturating_sub(offset).min(output.len() as u64) as usize;
        let base = self.profile.logical_base();
        let b = self.profile.payload_size() as u64;
        let mut done = 0;
        let mut observations = Vec::new();
        let mut zero_slots = 0;
        if offset < base && available > 0 {
            let control = self
                .control(io)?
                .ok_or_else(|| Error::corrupt("WAL header record is unusable"))?;
            let n = ((base - offset) as usize).min(available);
            let from = offset as usize;
            output[..n].copy_from_slice(&control.wal_header[from..from + n]);
            done = n;
        }
        while done < available {
            let at = offset
                .checked_add(done as u64)
                .ok_or_else(|| Error::new(TOOBIG, "read offset overflow"))?
                - base;
            let block = at / b;
            let within = (at % b) as usize;
            let (decoded, observation) = self.slot(io, block, physical)?;
            let n = (available - done).min(b as usize - within);
            output[done..done + n].copy_from_slice(&decoded.payload[within..within + n]);
            zero_slots += decoded.zero as u64;
            if let Some(value) = observation
                && observations.len() < MAX_READ_OBSERVATIONS
            {
                observations
                    .try_reserve(1)
                    .map_err(|_| Error::new(NOMEM, "allocation failed"))?;
                observations.push(value);
            }
            done += n;
        }
        Ok(ReadOutcome {
            short: available < output.len(),
            observations,
            zero_slots,
        })
    }
    /// Writes any frame header still waiting for a body as an ordinary partial write.
    pub fn flush(&mut self, io: &mut impl Physical) -> Result<()> {
        if let Some((block, header)) = self.pending.take() {
            let offset = self.profile.logical_base() + block * self.profile.payload_size() as u64;
            self.write_bytes(io, offset, &header)?;
        }
        Ok(())
    }
    pub fn write(
        &mut self,
        io: &mut impl Physical,
        offset: u64,
        input: &[u8],
    ) -> Result<Range<u64>> {
        if input.is_empty() {
            return Ok(0..0);
        }
        if self.profile.kind != Kind::Wal {
            return self.write_bytes(io, offset, input);
        }
        let base = self.profile.logical_base();
        let b = self.profile.payload_size() as u64;
        if offset < base {
            // SQLite writes the header whole, but the file contract is any bytes.
            let n = ((base - offset) as usize).min(input.len());
            self.flush(io)?;
            let mut control = self.control(io)?.unwrap_or_default();
            let from = offset as usize;
            control.sequence += 1;
            control.wal_header[from..from + n].copy_from_slice(&input[..n]);
            self.write_control(io, &control)?;
            self.last_commit = 0;
            return self.write(io, base, &input[n..]);
        }
        let block = (offset - base) / b;
        let within = ((offset - base) % b) as usize;
        if within == 0 && input.len() == WAL_FRAME_HEADER_SIZE {
            self.flush(io)?;
            let mut header = [0; WAL_FRAME_HEADER_SIZE];
            header.copy_from_slice(input);
            self.pending = Some((block, header));
            return Ok(block..block + 1);
        }
        if within == WAL_FRAME_HEADER_SIZE
            && input.len() == BLOCK_SIZE
            && let Some((_, header)) = self.pending.filter(|(pending, _)| *pending == block)
        {
            // A full replacement never reads the dead frame it overwrites.
            self.pending = None;
            let mut frame = zeros(b as usize)?;
            frame[..WAL_FRAME_HEADER_SIZE].copy_from_slice(&header);
            frame[WAL_FRAME_HEADER_SIZE..].copy_from_slice(input);
            let encoded = codec::encode_slot(&self.profile, block, &frame, b as usize)?;
            io.write(self.profile.slot_offset(block)?, &encoded)?;
            if header[4..8] != [0; 4] {
                self.last_commit = block + 1;
            }
            return Ok(block..block + 1);
        }
        self.flush(io)?;
        self.write_bytes(io, offset, input)
    }
    fn write_bytes(&self, io: &mut impl Physical, offset: u64, input: &[u8]) -> Result<Range<u64>> {
        let end = offset
            .checked_add(input.len() as u64)
            .ok_or_else(|| Error::new(TOOBIG, "write offset overflow"))?;
        if end > self.profile.max_logical_length() {
            return Err(Error::new(TOOBIG, "write exceeds format limit"));
        }
        let base = self.profile.logical_base();
        let first = offset - base;
        let end = end - base;
        let mut physical = io.size()?;
        let old_length = self.profile.logical_length(physical)?.saturating_sub(base);
        let b = self.profile.payload_size();
        let mut done = 0;
        while done < input.len() {
            let at = first + done as u64;
            let block = at / b as u64;
            let within = (at % b as u64) as usize;
            let n = (input.len() - done).min(b - within);
            let start = self.profile.slot_offset(block)?;
            let mut initialized = 0;
            let old_present = old_length.saturating_sub(block * b as u64).min(b as u64) as usize;
            // Replacing every present byte also replaces a final partial slot:
            // its absent suffix is already canonical zero, not preserved data.
            let mut payload = if within == 0 && n >= old_present {
                zeros(b)?
            } else if start < physical {
                let (prior, _) = self.slot(io, block, physical)?;
                initialized = prior.initialized;
                prior.payload
            } else {
                zeros(b)?
            };
            payload[within..within + n].copy_from_slice(&input[done..done + n]);
            initialized = initialized.max(within + n);
            self.validate_page_one(block, &payload)?;
            let encoded = codec::encode_slot(&self.profile, block, &payload, initialized)?;
            let logical_end = old_length.max(end);
            let present = (logical_end - block * b as u64).min(b as u64) as usize;
            let amount = self.profile.stored_length(present);
            io.write(start, &encoded[..amount])?;
            physical = physical.max(start + amount as u64);
            done += n;
        }
        Ok(first / b as u64..(end - 1) / b as u64 + 1)
    }
    fn validate_page_one(&self, block: u64, payload: &[u8]) -> Result<()> {
        if block == 0
            && self.profile.kind == Kind::Main
            && payload.starts_with(b"SQLite format 3\0")
        {
            let page_size = u16::from_be_bytes([payload[16], payload[17]]) as usize;
            if page_size != BLOCK_SIZE {
                return Err(Error::corrupt("SQLite page size differs from block size"));
            }
            // Pages with no room for a trailer would lose it, so such a database stops
            // at its first write rather than later at some full page.
            if payload[20] as usize != RESERVED_SIZE {
                return Err(Error::corrupt("SQLite reserved bytes differ"));
            }
        }
        Ok(())
    }
    pub fn truncate(&mut self, io: &mut impl Physical, length: u64) -> Result<()> {
        let length = self.profile.truncated_length(length);
        let new_physical = self.profile.physical_length(length)?;
        let physical = io.size()?;
        let old_length = self.profile.logical_length(physical)?;
        let b = self.profile.payload_size();
        let retained = length.saturating_sub(self.profile.logical_base());
        if length == old_length {
            return Ok(());
        }
        if length < old_length {
            let tail = (retained % b as u64) as usize;
            if tail != 0 {
                let block = retained / b as u64;
                let (mut old, _) = self.slot(io, block, physical)?;
                if !old.zero {
                    old.payload[tail..].fill(0);
                    let bytes = codec::encode_slot(
                        &self.profile,
                        block,
                        &old.payload,
                        old.initialized.min(tail),
                    )?;
                    let start = self.profile.slot_offset(block)?;
                    let present = (physical - start).min(self.profile.slot_size() as u64) as usize;
                    io.write(start, &bytes[..present])?;
                }
            }
        }
        io.truncate(new_physical)?;
        self.last_commit = self.last_commit.min(retained.div_ceil(b as u64));
        Ok(())
    }
    pub fn repair_prefix(&mut self, io: &mut impl Physical) -> Result<bool> {
        let physical = io.size()?;
        let prefix = codec::decode_prefix(&read_prefix(io, physical)?)?;
        if prefix.profile != self.profile {
            return Err(Error::corrupt("file prefix changed"));
        }
        if !prefix.repair.iter().any(|v| *v) {
            self.prefix_repair = [false; 2];
            return Ok(false);
        }
        let canonical = codec::encode_prefix(&prefix.profile)?;
        // Restore an unusable copy before touching the last recoverable copy.
        let first = if !prefix.usable[0] {
            0
        } else if !prefix.usable[1] {
            1
        } else if prefix.repair[0] {
            0
        } else {
            1
        };
        for index in [first, 1 - first] {
            if prefix.repair[index] {
                let offset = index * HEADER_REGION_SIZE;
                io.write(
                    offset as u64,
                    &canonical[offset..offset + HEADER_RECORD_SIZE],
                )?;
                io.sync(SYNC_FULL)?;
            }
        }
        self.prefix_repair = [false; 2];
        Ok(true)
    }
}
/// Reads each copy alone, so that a sector the storage refuses to return costs only that
/// copy. One left as zeros decodes as unusable, and the other copy carries on.
fn read_prefix(io: &mut impl Physical, physical: u64) -> Result<Vec<u8>> {
    let mut bytes = zeros(HEADER_SIZE)?;
    for (index, copy) in bytes.chunks_mut(HEADER_REGION_SIZE).enumerate() {
        let start = (index * HEADER_REGION_SIZE) as u64;
        let present = physical.saturating_sub(start).min(copy.len() as u64) as usize;
        if present > 0 {
            match io.read(start, &mut copy[..present]) {
                Err(error) if error.unreadable() => copy.fill(0),
                result => result?,
            }
        }
    }
    Ok(bytes)
}
fn control_offset(region: u64) -> u64 {
    HEADER_SIZE as u64 + region * CONTROL_REGION_SIZE as u64
}

#[cfg(test)]
#[path = "../tests/unit/file.rs"]
mod tests;
