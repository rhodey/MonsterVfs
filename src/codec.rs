use xxhash_rust::xxh3::Xxh3;

use crate::config::{ALIGNMENT, BLOCK_SIZE, Settings};
#[cfg(feature = "parity")]
use crate::error::IOERR_DATA;
use crate::error::{CANTOPEN, CORRUPT, Error, MISUSE, Result, zeros};
use crate::format::*;

#[cfg(feature = "copy")]
use crate::copy as build;
#[cfg(feature = "parity")]
use crate::parity as build;

#[derive(Debug)]
pub struct Decoded {
    pub payload: Vec<u8>,
    pub initialized: usize,
    pub needs_repair: bool,
    pub zero: bool,
    // Only part of the slot vouched for these bytes, so they may be an older page.
    pub rebuilt: bool,
}

#[derive(Clone, Debug)]
pub struct Prefix {
    pub profile: Profile,
    pub repair: [bool; 2],
    pub usable: [bool; 2],
}

/// One record of the WAL control region. The newest usable record wins.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Control {
    pub sequence: u64,
    pub wal_header: [u8; WAL_HEADER_SIZE],
    pub backfill_salts: [u8; 8],
    pub backfill_frames: u32,
}

pub fn hash(parts: &[&[u8]]) -> [u8; HASH_SIZE] {
    let mut hasher = Xxh3::new();
    for part in parts {
        hasher.update(part);
    }
    hasher.digest128().to_be_bytes()
}

pub(crate) fn copies_differ(copies: &[u8]) -> bool {
    let mut each = copies.chunks_exact(HASH_SIZE);
    each.next()
        .is_some_and(|first| each.any(|copy| copy != first))
}

pub(crate) fn matches_any(copies: &[u8], digest: &[u8; HASH_SIZE]) -> bool {
    copies.chunks_exact(HASH_SIZE).any(|copy| copy == digest)
}

pub(crate) fn store_copies(slot: &mut [u8], offsets: [usize; 3], hash: [u8; HASH_SIZE]) {
    for at in offsets {
        slot[at..at + HASH_SIZE].copy_from_slice(&hash);
    }
}

pub(crate) fn load_copies(slot: &[u8], offsets: [usize; 3]) -> [u8; HASH_COPIES_SIZE] {
    let mut copies = [0; HASH_COPIES_SIZE];
    for (copy, at) in copies.chunks_exact_mut(HASH_SIZE).zip(offsets) {
        copy.copy_from_slice(&slot[at..at + HASH_SIZE]);
    }
    copies
}

/// The bytes a checksum protects: where they belong, then the payload.
pub(crate) fn protected(
    profile: &Profile,
    block: u64,
    payload: &[u8],
    initialized: usize,
) -> Result<Vec<u8>> {
    let offset = profile.block_offset(block)?;
    let mut data = zeros::<u8>(METADATA_SIZE + profile.payload_size())?;
    data[..16].copy_from_slice(&profile.id);
    data[16..24].copy_from_slice(&offset.to_le_bytes());
    data[24..28].copy_from_slice(&(initialized as u32).to_le_bytes());
    data[METADATA_SIZE..METADATA_SIZE + payload.len()].copy_from_slice(payload);
    Ok(data)
}

/// Bytes a checksum proved still have to belong here, and say how much of them is real.
pub(crate) fn checked_initialized(profile: &Profile, offset: u64, data: &[u8]) -> Result<usize> {
    let stored_offset = u64::from_le_bytes(data[16..24].try_into().unwrap());
    let initialized = u32::from_le_bytes(data[24..28].try_into().unwrap()) as usize;
    if data[..16] != profile.id || stored_offset != offset {
        return Err(Error::corrupt("slot file identity or offset differs"));
    }
    if initialized > profile.payload_size()
        || data[28..32].iter().any(|&byte| byte != 0)
        || data[METADATA_SIZE + initialized..]
            .iter()
            .any(|&byte| byte != 0)
    {
        return Err(Error::corrupt(
            "slot initialized length or reserved fields are invalid",
        ));
    }
    Ok(initialized)
}

pub(crate) fn zero_slot(profile: &Profile) -> Result<Decoded> {
    Ok(Decoded {
        payload: zeros::<u8>(profile.payload_size())?,
        initialized: 0,
        needs_repair: false,
        zero: true,
        rebuilt: false,
    })
}

pub fn encode_prefix(profile: &Profile) -> Result<Vec<u8>> {
    profile.settings.validate()?;
    let mut data = [0u8; HEADER_DATA_SIZE];
    data[..8].copy_from_slice(&build::MAGIC);
    data[8..12].copy_from_slice(&build::VERSION.to_le_bytes());
    data[12..16].copy_from_slice(&(profile.kind as u32).to_le_bytes());
    data[16..32].copy_from_slice(&profile.id);
    data[32..36].copy_from_slice(&(BLOCK_SIZE as u32).to_le_bytes());
    data[36..40].copy_from_slice(&(profile.settings.parity_bytes as u32).to_le_bytes());
    data[40..44].copy_from_slice(&(ALIGNMENT as u32).to_le_bytes());
    data[44..48].copy_from_slice(&(HEADER_SIZE as u32).to_le_bytes());
    let record = build::protect(&data)?;
    let mut prefix = zeros::<u8>(HEADER_SIZE)?;
    for offset in [0, HEADER_REGION_SIZE] {
        prefix[offset..offset + HEADER_RECORD_SIZE].copy_from_slice(&record);
    }
    Ok(prefix)
}

fn parse_header(data: &[u8; HEADER_DATA_SIZE]) -> Result<Profile> {
    let word = |at: usize| u32::from_le_bytes(data[at..at + 4].try_into().unwrap());
    if data[..8] != build::MAGIC || word(8) != build::VERSION {
        return Err(Error::corrupt("file magic or version is invalid"));
    }
    if word(44) != HEADER_SIZE as u32 || data[48..].iter().any(|&byte| byte != 0) {
        return Err(Error::corrupt("file header reserved fields are invalid"));
    }
    let kind = Kind::try_from(word(12))?;
    if word(32) != BLOCK_SIZE as u32 || word(40) != ALIGNMENT as u32 {
        // Verified unsupported files are not unfinished creations we may replace.
        return Err(Error::new(
            CANTOPEN,
            "file requires unsupported block size or alignment",
        ));
    }
    let settings = Settings::new(word(36) as usize)
        .map_err(|err| Error::corrupt(format!("file settings are invalid: {}", err.message)))?;
    let mut id = [0u8; 16];
    id.copy_from_slice(&data[16..32]);
    Ok(Profile { settings, kind, id })
}

pub fn decode_prefix(bytes: &[u8]) -> Result<Prefix> {
    if bytes.len() != HEADER_SIZE {
        return Err(Error::corrupt("file prefix is incomplete"));
    }
    if other_build(bytes) {
        return Err(Error::corrupt("file belongs to the other MonsterVfs build"));
    }
    #[cfg(feature = "copy")]
    {
        decode_copied_prefix(bytes)
    }
    #[cfg(feature = "parity")]
    {
        let first = build::decode_copy(&bytes[..HEADER_RECORD_SIZE]);
        let second =
            build::decode_copy(&bytes[HEADER_REGION_SIZE..HEADER_REGION_SIZE + HEADER_RECORD_SIZE]);
        for result in [&first, &second] {
            if let Err(error) = result
                && error.code != IOERR_DATA
            {
                return Err(error.clone());
            }
        }
        let usable = [first.is_ok(), second.is_ok()];
        let (data, repair) = match (first, second) {
            (Ok((a, ar)), Ok((b, br))) => {
                if a != b {
                    return Err(Error::new(CORRUPT, "file prefix copies differ"));
                }
                (a, [ar, br])
            }
            (Ok((a, ar)), Err(_)) => (a, [ar, true]),
            (Err(_), Ok((b, br))) => (b, [true, br]),
            (Err(_), Err(_)) => return Err(Error::corrupt("both file prefix copies are unusable")),
        };
        Ok(Prefix {
            profile: parse_header(&data)?,
            repair,
            usable,
        })
    }
}

/// Conflicting identities need a majority and an intact region to anchor repair.
#[cfg(feature = "copy")]
fn decode_copied_prefix(bytes: &[u8]) -> Result<Prefix> {
    let regions = [
        build::decode_copies(&bytes[..HEADER_RECORD_SIZE]),
        build::decode_copies(&bytes[HEADER_REGION_SIZE..HEADER_REGION_SIZE + HEADER_RECORD_SIZE]),
    ];
    let candidates = regions.iter().flatten().flatten();
    let count = candidates.clone().count();
    if count == 0 {
        return Err(Error::corrupt("both file prefix copies are unusable"));
    }
    let data = candidates
        .clone()
        .find(|candidate| {
            let votes = candidates
                .clone()
                .filter(|value| value == candidate)
                .count();
            votes == count
                || (votes * 2 > count
                    && regions.iter().any(|region| {
                        region
                            .iter()
                            .all(|value| value.as_ref() == Some(*candidate))
                    }))
        })
        .ok_or_else(|| Error::new(CORRUPT, "file prefix copies differ"))?;
    let repair = regions.map(|region| region.iter().any(|value| value.as_ref() != Some(data)));
    let usable = regions.map(|region| {
        region.iter().flatten().any(|value| value == data)
            && region.iter().flatten().all(|value| value == data)
    });
    Ok(Prefix {
        profile: parse_header(data)?,
        repair,
        usable,
    })
}

/// Whichever build a file is opened with, it must not read as damage or get replaced.
pub fn other_build(prefix: &[u8]) -> bool {
    prefix.starts_with(&build::OTHER_MAGIC)
}

pub fn encode_control(id: &[u8; 16], control: &Control) -> Result<Vec<u8>> {
    let mut data = [0u8; HEADER_DATA_SIZE];
    data[..8].copy_from_slice(&CONTROL_MAGIC);
    data[8..16].copy_from_slice(&control.sequence.to_le_bytes());
    data[16..48].copy_from_slice(&control.wal_header);
    data[48..56].copy_from_slice(&control.backfill_salts);
    data[56..60].copy_from_slice(&control.backfill_frames.to_le_bytes());
    data[60..76].copy_from_slice(id);
    build::protect(&data)
}

/// Verify ownership before comparing independently protected control versions.
pub fn decode_controls(id: &[u8; 16], record: &[u8]) -> Result<[Option<Control>; 2]> {
    if record.iter().all(|&byte| byte == 0) {
        return Ok([None, None]);
    }
    #[cfg(feature = "copy")]
    let candidates = build::decode_copies(record);
    #[cfg(feature = "parity")]
    let candidates = match build::decode_copy(record) {
        Ok((data, _)) => [Some(data), None],
        Err(error) if error.code == IOERR_DATA => [None, None],
        Err(error) => return Err(error),
    };
    let mut controls = [None, None];
    for (control, candidate) in controls.iter_mut().zip(candidates) {
        let Some(data) = candidate else { continue };
        if data[..8] != CONTROL_MAGIC || data[76..].iter().any(|&byte| byte != 0) {
            continue;
        }
        if data[60..76] != *id {
            return Err(Error::new(CORRUPT, "WAL control file identity differs"));
        }
        let mut decoded = Control {
            sequence: u64::from_le_bytes(data[8..16].try_into().unwrap()),
            backfill_frames: u32::from_le_bytes(data[56..60].try_into().unwrap()),
            ..Default::default()
        };
        decoded.wal_header.copy_from_slice(&data[16..48]);
        decoded.backfill_salts.copy_from_slice(&data[48..56]);
        *control = Some(decoded);
    }
    Ok(controls)
}

pub fn encode_slot(
    profile: &Profile,
    block: u64,
    payload: &[u8],
    initialized: usize,
) -> Result<Vec<u8>> {
    profile.settings.validate()?;
    let block_size = profile.payload_size();
    if payload.len() > block_size
        || initialized > payload.len()
        || payload[initialized..].iter().any(|&byte| byte != 0)
    {
        return Err(Error::new(
            MISUSE,
            "slot payload or initialized length is invalid",
        ));
    }
    if payload
        .iter()
        .skip(block_size - profile.reserved_size())
        .any(|&byte| byte != 0)
    {
        return Err(Error::new(MISUSE, "reserved page bytes are not zero"));
    }
    if profile.kind == Kind::Temporary {
        encode_temporary(profile, block, payload, initialized)
    } else {
        build::encode_slot(profile, block, payload, initialized)
    }
}

pub fn decode_slot(profile: &Profile, block: u64, physical: &[u8]) -> Result<Decoded> {
    profile.settings.validate()?;
    if profile.kind == Kind::Temporary {
        decode_temporary(profile, block, physical)
    } else {
        build::decode_slot(profile, block, physical)
    }
}

/// Temporary files need detection and nothing more: SQLite discards them after a crash.
fn encode_temporary(
    profile: &Profile,
    block: u64,
    payload: &[u8],
    initialized: usize,
) -> Result<Vec<u8>> {
    let data = protected(profile, block, payload, initialized)?;
    let mut slot = zeros::<u8>(profile.slot_size())?;
    slot[..METADATA_SIZE].copy_from_slice(&data[..METADATA_SIZE]);
    store_copies(&mut slot, profile.hash_offsets().0, hash(&[&data]));
    slot[profile.prefix_size()..].copy_from_slice(&data[METADATA_SIZE..]);
    Ok(slot)
}

fn decode_temporary(profile: &Profile, block: u64, physical: &[u8]) -> Result<Decoded> {
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
    let copies = load_copies(physical, profile.hash_offsets().0);
    if !matches_any(&copies, &hash(&[&data])) {
        return Err(Error::corrupt("temporary data checksum differs"));
    }
    let initialized = checked_initialized(profile, offset, &data)?;
    let mut payload = zeros::<u8>(block_size)?;
    payload[..present].copy_from_slice(&data[METADATA_SIZE..METADATA_SIZE + present]);
    Ok(Decoded {
        payload,
        initialized: initialized.min(present),
        needs_repair: copies_differ(&copies) || initialized > present,
        zero: false,
        rebuilt: false,
    })
}

#[cfg(test)]
#[path = "../tests/unit/codec.rs"]
mod tests;
