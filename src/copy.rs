//! The `copy` build: each slot holds its page twice, in different sectors, with a trailer
//! in the reserved bytes of each copy. A copy proves itself alone.
use crate::codec::{Decoded, hash, zero_slot};
use crate::config::BLOCK_SIZE;
use crate::error::{Error, Result, zeros};
use crate::format::*;

pub const MAGIC: [u8; 8] = *b"MONSTCPY";
pub const OTHER_MAGIC: [u8; 8] = *b"MONSTVFS";
pub const VERSION: u32 = 1;

// Where each field sits in a trailer. The copy number keeps a drive from merging two
// identical blocks, and the checksum leaves it out so that both copies share one.
const INITIALIZED_AT: usize = HASH_SIZE;
const FRAME_HEADER_AT: usize = INITIALIZED_AT + 4;
const COPY_AT: usize = FRAME_HEADER_AT + WAL_FRAME_HEADER_SIZE;

/// Each record is written twice, so that damage to one needs no parity to survive.
pub fn protect(data: &[u8]) -> Result<Vec<u8>> {
    let digest = hash(&[data]);
    let mut record = zeros::<u8>(HEADER_RECORD_SIZE)?;
    for half in record.chunks_exact_mut(HEADER_RECORD_SIZE / 2) {
        half[..data.len()].copy_from_slice(data);
        half[data.len()..data.len() + HASH_SIZE].copy_from_slice(&digest);
    }
    Ok(record)
}

/// Each half supplies independent evidence; its caller decides agreement or version order.
pub fn decode_copies(record: &[u8]) -> [Option<[u8; HEADER_DATA_SIZE]>; 2] {
    std::array::from_fn(|index| {
        let start = index * (HEADER_RECORD_SIZE / 2);
        let data = &record[start..start + HEADER_DATA_SIZE];
        let digest = &record[start + HEADER_DATA_SIZE..start + HEADER_DATA_SIZE + HASH_SIZE];
        (hash(&[data]) == digest).then(|| data.try_into().unwrap())
    })
}

/// Everything in a copy except the checksum itself and the copy number. The identity and
/// offset are not stored: they only have to match, so a sector in the wrong place fails.
fn checksum(profile: &Profile, offset: u64, copy: &[u8]) -> [u8; HASH_SIZE] {
    let (page, trailer) = copy.split_at(copy.len() - RESERVED_SIZE);
    hash(&[
        &profile.id,
        &offset.to_le_bytes(),
        &trailer[INITIALIZED_AT..COPY_AT],
        page,
    ])
}

pub fn encode_slot(
    profile: &Profile,
    block: u64,
    payload: &[u8],
    initialized: usize,
) -> Result<Vec<u8>> {
    let offset = profile.block_offset(block)?;
    let size = BLOCK_SIZE;
    let (frame_header, page) = payload.split_at(payload.len().min(profile.frame_header_size()));
    let mut slot = zeros::<u8>(2 * size)?;
    let (first, second) = slot.split_at_mut(size);
    first[..page.len()].copy_from_slice(page);
    let trailer = &mut first[size - RESERVED_SIZE..];
    trailer[INITIALIZED_AT..FRAME_HEADER_AT].copy_from_slice(&(initialized as u32).to_le_bytes());
    trailer[FRAME_HEADER_AT..FRAME_HEADER_AT + frame_header.len()].copy_from_slice(frame_header);
    let digest = checksum(profile, offset, first);
    first[size - RESERVED_SIZE..][..HASH_SIZE].copy_from_slice(&digest);
    second.copy_from_slice(first);
    second[size - RESERVED_SIZE + COPY_AT] = 1;
    Ok(slot)
}

/// Two valid copies that differ are damage, not a choice. A synced slot cannot differ, so
/// a crash leaves this only in a WAL tail, which recovery cuts, or in a main page the WAL
/// still replaces. Anything else is a lost write, and nothing says which copy is older:
/// picking one could return an old page as current, and repair would make it permanent.
pub fn decode_slot(profile: &Profile, block: u64, physical: &[u8]) -> Result<Decoded> {
    let size = BLOCK_SIZE;
    if physical.len() != 2 * size {
        return Err(Error::corrupt("slot is incomplete"));
    }
    if physical.iter().all(|&byte| byte == 0) {
        return zero_slot(profile);
    }
    let offset = profile.block_offset(block)?;
    let (first, second) = physical.split_at(size);
    let copy_at = size - RESERVED_SIZE + COPY_AT;
    let proven =
        |copy: &[u8]| checksum(profile, offset, copy) == copy[size - RESERVED_SIZE..][..HASH_SIZE];
    let (copy, alone) = if first[..copy_at] == second[..copy_at] {
        (proven(first).then_some(first), false)
    } else {
        match (proven(first), proven(second)) {
            (true, true) => return Err(Error::corrupt("slot copies hold different writes")),
            (true, false) => (Some(first), true),
            (false, true) => (Some(second), true),
            (false, false) => (None, true),
        }
    };
    let copy = copy.ok_or_else(|| Error::corrupt("no slot copy is valid"))?;
    let trailer = &copy[size - RESERVED_SIZE..];
    let initialized_bytes = trailer[INITIALIZED_AT..FRAME_HEADER_AT].try_into().unwrap();
    let initialized = u32::from_le_bytes(initialized_bytes) as usize;
    let width = profile.payload_size();
    if initialized > width {
        return Err(Error::corrupt("slot initialized length is invalid"));
    }
    let frame_header = profile.frame_header_size();
    let mut payload = zeros::<u8>(width)?;
    payload[..frame_header]
        .copy_from_slice(&trailer[FRAME_HEADER_AT..FRAME_HEADER_AT + frame_header]);
    payload[frame_header..frame_header + size - RESERVED_SIZE]
        .copy_from_slice(&copy[..size - RESERVED_SIZE]);
    Ok(Decoded {
        payload,
        initialized,
        needs_repair: alone || first[copy_at..] != [0; 4] || second[copy_at..] != [1, 0, 0, 0],
        zero: false,
        rebuilt: alone,
    })
}

#[cfg(test)]
#[path = "../tests/unit/copy.rs"]
mod tests;
