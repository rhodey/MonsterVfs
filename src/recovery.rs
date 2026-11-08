//! Read-only planning for an authorized WAL cutoff; SQLite owns exclusion.
use crate::{
    config::BLOCK_SIZE,
    error::{CORRUPT_BACKFILLED, Error, Result, TOOBIG},
    file::{MAX_READ_OBSERVATIONS, Mapped, Observation},
    format::{HEADER_SIZE, WAL_FRAME_HEADER_SIZE, WAL_HEADER_SIZE},
    native::Physical,
};

#[derive(Debug, Eq, PartialEq)]
pub struct Cutoff {
    pub offset: u64,
    // Reports what the cut discards. It never decides whether to cut.
    pub later_frames: u64,
    pub lost_commits: u64,
}
pub struct Scan {
    pub cutoff: Option<Cutoff>,
    pub observations: Vec<Observation>,
}
pub fn scan(file: &Mapped, io: &mut impl Physical) -> Result<Scan> {
    let physical = io.size()?;
    if physical < HEADER_SIZE as u64 {
        return Err(Error::corrupt("incomplete WAL prefix"));
    }
    let control = file.control(io)?;
    if physical <= file.profile.content_start() {
        return Ok(Scan {
            cutoff: None,
            observations: Vec::new(),
        });
    }
    // SQLite syncs its header before writing any frame, so a crash cannot leave
    // frames without one. Only damage can, and then the watermark is lost too.
    let control = control
        .ok_or_else(|| Error::new(CORRUPT_BACKFILLED, "WAL frames have no usable header"))?;
    let salts = &control.wal_header[16..24];
    let backfilled = if control.backfill_salts == salts {
        control.backfill_frames as u64
    } else {
        0
    };
    let mut observations = Vec::new();
    let mut block = 0;
    let mut log_ended = false;
    loop {
        let start = file.profile.slot_offset(block)?;
        if start >= physical {
            // Frames through the watermark were synced, so a crash cannot have lost them.
            if block < backfilled {
                return Err(Error::new(
                    CORRUPT_BACKFILLED,
                    "WAL ends below the backfill watermark",
                ));
            }
            return Ok(Scan {
                cutoff: None,
                observations,
            });
        }
        let result = if physical - start < file.profile.stored_length(1) as u64 {
            Err(Error::corrupt("incomplete WAL slot prefix"))
        } else {
            file.slot(io, block, physical)
        };
        let damaged = match result {
            Ok((decoded, observation)) => {
                if let Some(value) = observation
                    && observations.len() < MAX_READ_OBSERVATIONS
                {
                    observations.push(value);
                }
                // SQLite stops at a zero frame, so it is the same loss as damage.
                let mut lost = decoded.zero;
                // It stops at another generation's frame too, which normally just ends
                // the log. Not if only its page sector vouched for it, or if a commit
                // after it finished: that commit's sync covered this frame, so a device
                // lost it, and the watermark must still get its say.
                if !lost && decoded.payload[8..16] != *salts {
                    lost = decoded.rebuilt
                        || (!log_ended
                            && discarded(file, io, block + 1, physical, &control.wal_header).1
                                != 0);
                    log_ended = true;
                }
                lost
            }
            Err(error) if error.unreadable() => true,
            Err(error) => return Err(error),
        };
        if damaged {
            // Frames through the watermark may be in main already. Retained
            // frames would override only some of those pages: a torn transaction.
            if block < backfilled {
                return Err(Error::new(
                    CORRUPT_BACKFILLED,
                    "WAL damage below the backfill watermark",
                ));
            }
            let (later_frames, lost_commits) =
                discarded(file, io, block + 1, physical, &control.wal_header);
            return Ok(Scan {
                cutoff: Some(Cutoff {
                    offset: start,
                    later_frames,
                    lost_commits,
                }),
                observations,
            });
        }
        block = block
            .checked_add(1)
            .ok_or_else(|| Error::new(TOOBIG, "WAL block overflow"))?;
    }
}
/// Where SQLite's own recovery ends: the last commit reached by following the salts and
/// the checksum chain from the header. A private wal-index holds the same answer.
pub fn last_commit(file: &Mapped, io: &mut impl Physical) -> Result<u64> {
    let physical = io.size()?;
    let Some(control) = file.control(io)? else {
        return Ok(0);
    };
    let salts = &control.wal_header[16..24];
    let big_endian = control.wal_header[3] & 1 == 1;
    let mut sum = control.wal_header[24..32].to_vec();
    let mut last = 0;
    for block in 0.. {
        if file.profile.slot_offset(block)? >= physical {
            break;
        }
        let frame = match file.slot(io, block, physical) {
            Ok((decoded, _)) => decoded.payload,
            Err(error) if error.unreadable() => break,
            Err(error) => return Err(error),
        };
        if frame[8..16] != *salts || !continues(big_endian, &sum, &frame) {
            break;
        }
        sum = frame[16..24].to_vec();
        if frame[4..8] != [0; 4] {
            last = block + 1;
        }
    }
    Ok(last)
}
/// Counts intact frames of the current generation that a cut would remove, and the
/// commits among them that certainly finished. A frame that continues a commit's checksum
/// chain was written after that commit returned, and WAL/FULL syncs a commit before it
/// returns, so a crash cannot have torn anything ahead of it. The last commit proves
/// nothing: it looks just like a torn tail. Stale frames from an earlier crash never
/// chain onto a commit, so they prove nothing either.
fn discarded(
    file: &Mapped,
    io: &mut impl Physical,
    from: u64,
    physical: u64,
    header: &[u8; WAL_HEADER_SIZE],
) -> (u64, u64) {
    let salts = &header[16..24];
    let big_endian = header[3] & 1 == 1;
    let (mut frames, mut finished) = (0, 0);
    let mut earlier: Option<Vec<u8>> = None;
    for block in from.. {
        let Ok(start) = file.profile.slot_offset(block) else {
            break;
        };
        if start >= physical {
            break;
        }
        let frame = file
            .slot(io, block, physical)
            .ok()
            .map(|(decoded, _)| decoded.payload)
            .filter(|payload| payload[8..16] == *salts);
        if let Some(frame) = &frame {
            frames += 1;
            finished += earlier.as_ref().is_some_and(|prior| {
                prior[4..8] != [0; 4] && continues(big_endian, &prior[16..24], frame)
            }) as u64;
        }
        earlier = frame;
    }
    (frames, finished)
}
/// SQLite's running frame checksum, carried on from the checksum before it.
fn continues(big_endian: bool, sum: &[u8], frame: &[u8]) -> bool {
    let stored =
        |bytes: &[u8], at: usize| u32::from_be_bytes(bytes[at..at + 4].try_into().unwrap());
    let word = |bytes: &[u8]| {
        let value = bytes.try_into().unwrap();
        if big_endian {
            u32::from_be_bytes(value)
        } else {
            u32::from_le_bytes(value)
        }
    };
    let (mut a, mut b) = (stored(sum, 0), stored(sum, 4));
    let page = frame[WAL_FRAME_HEADER_SIZE..].chunks_exact(8);
    for pair in frame[..8].chunks_exact(8).chain(page) {
        a = a.wrapping_add(word(&pair[..4])).wrapping_add(b);
        b = b.wrapping_add(word(&pair[4..])).wrapping_add(a);
    }
    a == stored(frame, 16) && b == stored(frame, 20)
}
pub fn validate_header(bytes: &[u8; WAL_HEADER_SIZE]) -> Result<()> {
    let be = |i| u32::from_be_bytes(bytes[i..i + 4].try_into().unwrap());
    let magic = be(0);
    if magic & !1 != 0x377f0682 || be(4) != 3_007_000 || be(8) as usize != BLOCK_SIZE {
        return Err(Error::corrupt("SQLite WAL header is illegal"));
    }
    let word = |i| {
        let value = bytes[i..i + 4].try_into().unwrap();
        if magic & 1 == 0 {
            u32::from_le_bytes(value)
        } else {
            u32::from_be_bytes(value)
        }
    };
    let (mut a, mut b) = (0u32, 0u32);
    for i in (0..24).step_by(8) {
        a = a.wrapping_add(word(i)).wrapping_add(b);
        b = b.wrapping_add(word(i + 4)).wrapping_add(a);
    }
    if a != be(24) || b != be(28) {
        return Err(Error::corrupt("SQLite WAL header checksum differs"));
    }
    Ok(())
}

#[cfg(test)]
#[path = "../tests/unit/recovery.rs"]
mod tests;
