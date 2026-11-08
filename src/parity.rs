//! The `parity` build: each slot holds a page, checksums and Reed-Solomon parity.
use crate::codec::{
    Decoded, checked_initialized, copies_differ, hash, load_copies, matches_any, protected,
    store_copies, zero_slot,
};
use crate::config::BLOCK_SIZE;
use crate::error::{Error, Result, zeros};
use crate::fec;
use crate::format::*;

pub const MAGIC: [u8; 8] = *b"MONSTVFS";
pub const OTHER_MAGIC: [u8; 8] = *b"MONSTCPY";
pub const VERSION: u32 = 1;
const HEADER_PARITY_SIZE: usize = 32;

fn store_hash(dst: &mut [u8], hash: [u8; HASH_SIZE]) {
    for copy in dst.chunks_exact_mut(HASH_SIZE) {
        copy.copy_from_slice(&hash);
    }
}

/// The end of a page, which SQLite leaves to us. With the page around it, this is all
/// the sector needs to prove itself, so a slot survives losing its metadata sector.
fn trailer(profile: &Profile, data: &[u8], digest: [u8; HASH_SIZE]) -> [u8; RESERVED_SIZE] {
    let mut trailer = [0; RESERVED_SIZE];
    trailer[..HASH_SIZE].copy_from_slice(&digest);
    trailer[16..20].copy_from_slice(&data[24..28]);
    if profile.kind == Kind::Wal {
        trailer[20..44]
            .copy_from_slice(&data[METADATA_SIZE..METADATA_SIZE + WAL_FRAME_HEADER_SIZE]);
    }
    trailer
}

/// Rebuilds the protected bytes from the page sector alone. Everything else in them is
/// already known, and the hash proves the result, so nothing here is trusted unproven.
fn from_trailer(profile: &Profile, offset: u64, physical: &[u8]) -> Result<Option<Vec<u8>>> {
    let (page, trailer) = physical.split_at(physical.len() - RESERVED_SIZE);
    let frame_header = profile.frame_header_size();
    let page = &page[page.len() + RESERVED_SIZE - BLOCK_SIZE..];
    let mut data = zeros::<u8>(METADATA_SIZE + profile.payload_size())?;
    data[..16].copy_from_slice(&profile.id);
    data[16..24].copy_from_slice(&offset.to_le_bytes());
    data[24..28].copy_from_slice(&trailer[16..20]);
    data[METADATA_SIZE..METADATA_SIZE + frame_header]
        .copy_from_slice(&trailer[20..20 + frame_header]);
    data[METADATA_SIZE + frame_header..METADATA_SIZE + frame_header + page.len()]
        .copy_from_slice(page);
    Ok((hash(&[&data]) == trailer[..HASH_SIZE]).then_some(data))
}

/// If the prefix and the page sector each prove a different write, the device lost one of
/// them and nothing says which. Choosing could return an old page as if it were current,
/// and repair would then make that permanent, so this is damage. Parity that matches its
/// checksum is what shows a prefix really was written: a filled or erased sector has none.
fn conflict(
    profile: &Profile,
    offset: u64,
    physical: &[u8],
    copies: &[u8],
    parity_copies: &[u8],
) -> Result<()> {
    let parity = &physical[PARITY_OFFSET..PARITY_OFFSET + profile.parity_bytes()];
    let trailer_hash = &physical[physical.len() - RESERVED_SIZE..][..HASH_SIZE];
    let prefix_written = matches_any(parity_copies, &hash(&[parity]));
    if prefix_written
        && !copies
            .chunks_exact(HASH_SIZE)
            .any(|copy| copy == trailer_hash)
        && from_trailer(profile, offset, physical)?.is_some()
    {
        return Err(Error::corrupt("slot sectors hold different writes"));
    }
    Ok(())
}

pub fn protect(data: &[u8]) -> Result<Vec<u8>> {
    let parity_bytes = HEADER_PARITY_SIZE;
    let parity = fec::encode(data, parity_bytes)?;
    let mut record = zeros::<u8>(data.len() + 2 * HASH_COPIES_SIZE + parity_bytes)?;
    let hash_at = data.len();
    let parity_at = hash_at + HASH_COPIES_SIZE;
    let parity_hash_at = parity_at + parity_bytes;
    record[..data.len()].copy_from_slice(data);
    store_hash(&mut record[hash_at..parity_at], hash(&[data]));
    record[parity_at..parity_hash_at].copy_from_slice(&parity);
    store_hash(&mut record[parity_hash_at..], hash(&[&parity]));
    Ok(record)
}

// A 128-bit match against any stored copy proves the bytes, so no vote is needed.
// Copies only ever confirm a checksum; a decoder result cannot create one. `before_correction`
// can stop a correction that would turn good bytes into the copies' older ones.
fn verify(
    data: &mut [u8],
    copies: &[u8],
    parity: &mut [u8],
    parity_copies: &[u8],
    before_correction: impl FnOnce() -> Result<()>,
) -> Result<bool> {
    let mut repair = copies_differ(copies);
    if !matches_any(copies, &hash(&[data])) {
        before_correction()?;
        fec::correct(data, parity)?;
        if !matches_any(copies, &hash(&[data])) {
            return Err(Error::corrupt("corrected data checksum differs"));
        }
        repair = true;
    }
    repair |= copies_differ(parity_copies) || !matches_any(parity_copies, &hash(&[parity]));
    Ok(repair)
}

pub fn decode_copy(record: &[u8]) -> Result<([u8; HEADER_DATA_SIZE], bool)> {
    let mut data = [0u8; HEADER_DATA_SIZE];
    data.copy_from_slice(&record[..HEADER_DATA_SIZE]);
    let copies_at = HEADER_DATA_SIZE;
    let parity_at = copies_at + HASH_COPIES_SIZE;
    let parity_hash_at = parity_at + HEADER_PARITY_SIZE;
    let mut parity = [0u8; HEADER_PARITY_SIZE];
    parity.copy_from_slice(&record[parity_at..parity_hash_at]);
    let repair = verify(
        &mut data,
        &record[copies_at..parity_at],
        &mut parity,
        &record[parity_hash_at..],
        || Ok(()),
    )?;
    Ok((data, repair))
}

pub fn encode_slot(
    profile: &Profile,
    block: u64,
    payload: &[u8],
    initialized: usize,
) -> Result<Vec<u8>> {
    let data = protected(profile, block, payload, initialized)?;
    let mut slot = zeros::<u8>(profile.slot_size())?;
    slot[..METADATA_SIZE].copy_from_slice(&data[..METADATA_SIZE]);
    let (data_at, parity_at) = profile.hash_offsets();
    let digest = hash(&[&data]);
    store_copies(&mut slot, data_at, digest);
    let parity = fec::encode(&data, profile.parity_bytes())?;
    slot[PARITY_OFFSET..PARITY_OFFSET + parity.len()].copy_from_slice(&parity);
    store_copies(&mut slot, parity_at, hash(&[&parity]));
    slot[profile.prefix_size()..].copy_from_slice(&data[METADATA_SIZE..]);
    if initialized == profile.payload_size() {
        let at = slot.len() - RESERVED_SIZE;
        slot[at..].copy_from_slice(&trailer(profile, &data, digest));
    }
    Ok(slot)
}

pub fn decode_slot(profile: &Profile, block: u64, physical: &[u8]) -> Result<Decoded> {
    let offset = profile.block_offset(block)?;
    let prefix = profile.prefix_size();
    if physical.len() <= prefix || physical.len() > profile.slot_size() {
        return Err(Error::corrupt("slot physical length is invalid"));
    }
    let present = physical.len() - prefix;
    let block_size = profile.payload_size();
    if physical.iter().all(|&byte| byte == 0) {
        return zero_slot(profile);
    }
    let mut data = zeros::<u8>(METADATA_SIZE + block_size)?;
    data[..METADATA_SIZE].copy_from_slice(&physical[..METADATA_SIZE]);
    data[METADATA_SIZE..METADATA_SIZE + present].copy_from_slice(&physical[prefix..]);
    // The page has zeros under its trailer, and the checksums and parity cover those.
    data[METADATA_SIZE + block_size - RESERVED_SIZE..].fill(0);
    let (data_at, parity_at) = profile.hash_offsets();
    let mut parity = zeros::<u8>(profile.parity_bytes())?;
    parity.copy_from_slice(&physical[PARITY_OFFSET..PARITY_OFFSET + profile.parity_bytes()]);
    let complete = present == block_size;
    let copies = load_copies(physical, data_at);
    let parity_copies = load_copies(physical, parity_at);
    let mut conflicted = false;
    let verified = verify(&mut data, &copies, &mut parity, &parity_copies, || {
        if complete {
            let result = conflict(profile, offset, physical, &copies, &parity_copies);
            conflicted = result.is_err();
            result
        } else {
            Ok(())
        }
    });
    let mut rebuilt = false;
    let mut needs_repair = match verified {
        Ok(repair) => repair,
        Err(error) if complete && error.unreadable() && !conflicted => {
            data = from_trailer(profile, offset, physical)?.ok_or(error)?;
            rebuilt = true;
            true
        }
        Err(error) => return Err(error),
    };
    let initialized = checked_initialized(profile, offset, &data)?;
    if complete && initialized == block_size {
        let expected = trailer(profile, &data, hash(&[&data]));
        needs_repair |= physical[physical.len() - RESERVED_SIZE..] != expected;
    }
    // A repaired old codeword can extend beyond a newly persisted truncate.
    // Verify first, then honor physical EOF so later extension cannot resurrect it.
    needs_repair |= initialized > present;
    let mut payload = zeros::<u8>(block_size)?;
    payload[..present].copy_from_slice(&data[METADATA_SIZE..METADATA_SIZE + present]);
    Ok(Decoded {
        payload,
        initialized: initialized.min(present),
        needs_repair,
        zero: false,
        rebuilt,
    })
}

#[cfg(test)]
#[path = "../tests/unit/parity.rs"]
mod tests;
