use super::*;
use crate::codec::{decode_slot as decode, encode_slot as encode};
use crate::config::Settings;

fn profile(kind: Kind) -> Profile {
    Profile::new(Settings::default(), kind, [0x35; 16]).unwrap()
}

fn payload() -> Vec<u8> {
    let mut page: Vec<u8> = (0..4096).map(|i| (i * 17 + i / 31) as u8).collect();
    page[4096 - RESERVED_SIZE..].fill(0);
    page
}

#[test]
fn metadata_payload_and_parity_errors_are_repaired() {
    let profile = profile(Kind::Main);
    let original = payload();
    let slot = encode(&profile, 4, &original, 4096).unwrap();
    let mut damaged = slot.clone();
    for at in [
        0,
        16,
        24,
        28,
        profile.prefix_size(),
        profile.prefix_size() + 31,
        PARITY_OFFSET,
        PARITY_OFFSET + 30,
    ] {
        damaged[at] ^= 0x79;
    }
    let decoded = decode(&profile, 4, &damaged).unwrap();
    assert_eq!(decoded.payload, original);
    assert!(decoded.needs_repair);
}

#[test]
fn good_payload_is_returned_even_with_unrecoverable_parity_protection() {
    let profile = profile(Kind::Main);
    let original = payload();
    let mut slot = encode(&profile, 0, &original, 4096).unwrap();
    for byte in &mut slot[PARITY_OFFSET..PARITY_OFFSET + 32] {
        *byte ^= 0xa7;
    }
    let (_, parity_hashes) = profile.hash_offsets();
    slot[parity_hashes[0]] ^= 1;
    slot[parity_hashes[1]] ^= 2;
    slot[parity_hashes[2]] ^= 4;
    let decoded = decode(&profile, 0, &slot).unwrap();
    assert_eq!(decoded.payload, original);
    assert!(decoded.needs_repair);
}

#[test]
fn any_matching_checksum_copy_proves_the_data_and_degraded_copies_are_reported() {
    let profile = profile(Kind::Wal);
    let original = payload();
    let slot = encode(&profile, 0, &original, 4096).unwrap();
    let (hashes, _) = profile.hash_offsets();
    let mut damaged = slot.clone();
    damaged[hashes[0]] ^= 1;
    assert!(decode(&profile, 0, &damaged).unwrap().needs_repair);
    damaged[hashes[1]] ^= 2;
    let decoded = decode(&profile, 0, &damaged).unwrap();
    assert_eq!(&decoded.payload[..4096], &original[..]);
    assert!(decoded.needs_repair);

    // Two copies agreeing on the wrong value cannot outvote the proof.
    let mut outvoted = slot.clone();
    outvoted[hashes[0]] ^= 1;
    outvoted[hashes[1]] ^= 1;
    assert!(decode(&profile, 0, &outvoted).unwrap().needs_repair);

    let mut none_left = slot;
    for at in hashes {
        none_left[at] ^= 4;
    }
    assert!(decode(&profile, 0, &none_left).is_err());
}

#[test]
fn correction_is_accepted_only_against_a_stored_checksum() {
    let profile = profile(Kind::Main);
    let mut slot = encode(&profile, 0, &payload(), 4096).unwrap();
    let (hashes, _) = profile.hash_offsets();
    slot[profile.prefix_size()] ^= 1;
    slot[hashes[0]] ^= 1;
    slot[hashes[1]] ^= 2;
    assert!(decode(&profile, 0, &slot).unwrap().needs_repair);
    slot[hashes[2]] ^= 4;
    assert!(decode(&profile, 0, &slot).is_err());
}

#[test]
fn excessive_damage_is_rejected() {
    let profile = profile(Kind::Main);
    let mut slot = encode(&profile, 0, &payload(), 4096).unwrap();
    for i in 0..20 {
        slot[profile.prefix_size() + 2 * i] ^= (i + 1) as u8;
    }
    assert!(decode(&profile, 0, &slot).is_err());
}

#[test]
fn location_identity_and_semantic_metadata_are_checked() {
    let profile = profile(Kind::Main);
    let slot = encode(&profile, 2, &payload(), 4096).unwrap();
    assert!(decode(&profile, 3, &slot).is_err());
    let mut other = profile;
    other.id[0] ^= 1;
    assert!(decode(&other, 2, &slot).is_err());

    let mut data = vec![0; METADATA_SIZE + 4096];
    data[..16].copy_from_slice(&profile.id);
    data[16..24].copy_from_slice(&8192u64.to_le_bytes());
    data[24..28].copy_from_slice(&4097u32.to_le_bytes());
    let parity = fec::encode(&data, 32).unwrap();
    let mut invalid = vec![0; profile.slot_size()];
    invalid[..METADATA_SIZE].copy_from_slice(&data[..METADATA_SIZE]);
    let (hashes, parity_hashes) = profile.hash_offsets();
    store_copies(&mut invalid, hashes, hash(&[&data]));
    invalid[PARITY_OFFSET..PARITY_OFFSET + 32].copy_from_slice(&parity);
    store_copies(&mut invalid, parity_hashes, hash(&[&parity]));
    assert!(decode(&profile, 2, &invalid).is_err());
}

fn frame() -> Vec<u8> {
    let mut frame = vec![0x6b; WAL_FRAME_HEADER_SIZE];
    frame.extend(payload());
    frame
}

#[test]
fn the_page_sector_survives_losing_the_metadata_sector() {
    for (kind, original) in [(Kind::Main, payload()), (Kind::Wal, frame())] {
        let profile = profile(kind);
        let slot = encode(&profile, 5, &original, original.len()).unwrap();
        let page = profile.slot_size() - 4096;
        for filler in [0, 0xa5] {
            let mut lost = slot.clone();
            lost[..page].fill(filler);
            let decoded = decode(&profile, 5, &lost).unwrap();
            assert_eq!(decoded.payload, original);
            assert_eq!(decoded.initialized, original.len());
            assert!(decoded.needs_repair);
            assert!(!decoded.zero);
            assert!(decoded.rebuilt);
        }
    }
}

#[test]
fn the_page_sector_proves_itself_and_its_place() {
    let profile = profile(Kind::Wal);
    let slot = encode(&profile, 5, &frame(), 4120).unwrap();
    let page = profile.slot_size() - 4096;
    let mut lost = slot.clone();
    lost[..page].fill(0);
    assert!(decode(&profile, 6, &lost).is_err());
    let mut other = profile;
    other.id[0] ^= 1;
    assert!(decode(&other, 5, &lost).is_err());
    let mut flipped = lost.clone();
    flipped[page + 100] ^= 1;
    assert!(decode(&profile, 5, &flipped).is_err());
    let mut header = lost;
    header[profile.slot_size() - RESERVED_SIZE + 25] ^= 1;
    assert!(decode(&profile, 5, &header).is_err());
}

#[test]
fn sectors_from_two_different_writes_are_damage_and_not_a_choice() {
    let profile = profile(Kind::Main);
    let page = profile.slot_size() - 4096;
    let old_page = payload();
    let mut new_page = old_page.clone();
    new_page[100..200].fill(0x77);
    // Only two symbols apart, which parity alone would happily turn back into the old page.
    let mut near_page = old_page.clone();
    near_page[100] ^= 1;
    for other in [new_page, near_page] {
        let old = encode(&profile, 2, &old_page, 4096).unwrap();
        let new = encode(&profile, 2, &other, 4096).unwrap();
        let mut lost_page_write = new.clone();
        lost_page_write[page..].copy_from_slice(&old[page..]);
        assert!(decode(&profile, 2, &lost_page_write).is_err());
        let mut lost_prefix_write = new.clone();
        lost_prefix_write[..page].copy_from_slice(&old[..page]);
        assert!(decode(&profile, 2, &lost_prefix_write).is_err());
    }
}

#[test]
fn hash_copies_sit_apart_so_one_run_of_damage_cannot_take_them_all() {
    for kind in [Kind::Main, Kind::Wal] {
        let profile = profile(kind);
        let original = if kind == Kind::Wal {
            frame()
        } else {
            payload()
        };
        let slot = encode(&profile, 1, &original, original.len()).unwrap();
        let (hashes, parity_hashes) = profile.hash_offsets();
        assert!(hashes.windows(2).all(|pair| pair[1] - pair[0] > 1024));
        assert_eq!(parity_hashes, hashes.map(|at| at + HASH_SIZE));
        let mut burst = slot.clone();
        burst[32..160].fill(0xff);
        let decoded = decode(&profile, 1, &burst).unwrap();
        assert_eq!(decoded.payload, original);
        assert!(decoded.needs_repair);
    }
}

#[test]
fn a_damaged_trailer_costs_only_a_repair() {
    let profile = profile(Kind::Wal);
    let original = frame();
    let slot = encode(&profile, 3, &original, 4120).unwrap();
    assert!(!decode(&profile, 3, &slot).unwrap().needs_repair);
    for at in 0..RESERVED_SIZE {
        let mut damaged = slot.clone();
        damaged[profile.slot_size() - RESERVED_SIZE + at] ^= 0x41;
        let decoded = decode(&profile, 3, &damaged).unwrap();
        assert_eq!(decoded.payload, original, "trailer byte {at}");
        assert!(decoded.needs_repair, "trailer byte {at}");
    }
}

#[test]
fn only_fully_initialized_slots_have_a_trailer() {
    let profile = profile(Kind::Main);
    let original = payload();
    let slot = encode(&profile, 0, &original[..100], 100).unwrap();
    assert!(
        slot[profile.slot_size() - RESERVED_SIZE..]
            .iter()
            .all(|&b| b == 0)
    );
    assert!(!decode(&profile, 0, &slot).unwrap().needs_repair);
    let mut lost = slot;
    lost[..profile.prefix_size()].fill(0);
    assert!(decode(&profile, 0, &lost).is_err());
}

#[test]
fn sparse_zero_slots_require_every_present_byte_to_be_zero() {
    let profile = profile(Kind::Main);
    let mut slot = vec![0; profile.slot_size()];
    let decoded = decode(&profile, 0, &slot).unwrap();
    assert!(decoded.zero);
    assert!(!decoded.needs_repair);
    assert!(decoded.payload.iter().all(|&b| b == 0));
    assert!(
        decode(&profile, 0, &slot[..profile.prefix_size() + 1])
            .unwrap()
            .zero
    );
    assert!(decode(&profile, 0, &slot[..profile.prefix_size()]).is_err());
    slot[profile.prefix_size() - 1] = 1;
    assert!(decode(&profile, 0, &slot).is_err());
}

#[test]
fn interrupted_truncate_repairs_retained_bytes_without_tail_resurrection() {
    let profile = profile(Kind::Wal);
    let original = payload();
    let slot = encode(&profile, 0, &original, 4096).unwrap();
    let present = 4088;
    let decoded = decode(&profile, 0, &slot[..profile.prefix_size() + present]).unwrap();
    assert_eq!(&decoded.payload[..present], &original[..present]);
    assert!(decoded.payload[present..].iter().all(|&b| b == 0));
    assert_eq!(decoded.initialized, present);
    assert!(decoded.needs_repair);
    let replacement = encode(&profile, 0, &decoded.payload, decoded.initialized).unwrap();
    assert_eq!(
        decode(&profile, 0, &replacement).unwrap().payload,
        decoded.payload
    );
}
