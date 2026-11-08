use super::*;
use crate::codec::{decode_prefix, decode_slot as decode, encode_prefix, encode_slot as encode};
use crate::config::Settings;

const SIZE: usize = 4096;

fn profile(kind: Kind) -> Profile {
    Profile::new(Settings::default(), kind, [0x35; 16]).unwrap()
}

fn page() -> Vec<u8> {
    let mut page: Vec<u8> = (0..SIZE).map(|i| (i * 17 + i / 31) as u8).collect();
    page[SIZE - RESERVED_SIZE..].fill(0);
    page
}

fn frame() -> Vec<u8> {
    let mut frame = vec![0x6b; WAL_FRAME_HEADER_SIZE];
    frame.extend(page());
    frame
}

fn each_kind() -> [(Kind, Vec<u8>); 2] {
    [(Kind::Main, page()), (Kind::Wal, frame())]
}

#[test]
fn a_slot_is_two_copies_that_differ_only_in_their_number() {
    for (kind, original) in each_kind() {
        let profile = profile(kind);
        let slot = encode(&profile, 5, &original, original.len()).unwrap();
        assert_eq!(slot.len(), 2 * SIZE);
        let (first, second) = slot.split_at(SIZE);
        let differing: Vec<_> = (0..SIZE).filter(|&at| first[at] != second[at]).collect();
        assert_eq!(differing, [SIZE - 4]);
        let decoded = decode(&profile, 5, &slot).unwrap();
        assert_eq!(decoded.payload, original);
        assert_eq!(decoded.initialized, original.len());
        assert!(!decoded.needs_repair);
        assert!(!decoded.rebuilt);
    }
}

#[test]
fn either_copy_survives_losing_the_other() {
    for (kind, original) in each_kind() {
        let profile = profile(kind);
        let slot = encode(&profile, 5, &original, original.len()).unwrap();
        for lost in [0..SIZE, SIZE..2 * SIZE] {
            for filler in [0, 0xa5] {
                let mut damaged = slot.clone();
                damaged[lost.clone()].fill(filler);
                let decoded = decode(&profile, 5, &damaged).unwrap();
                assert_eq!(decoded.payload, original);
                assert_eq!(decoded.initialized, original.len());
                assert!(decoded.needs_repair);
                assert!(decoded.rebuilt);
                assert!(!decoded.zero);
            }
        }
    }
}

#[test]
fn a_flipped_bit_anywhere_in_either_copy_costs_only_that_copy() {
    let profile = profile(Kind::Wal);
    let original = frame();
    let slot = encode(&profile, 2, &original, original.len()).unwrap();
    for at in (0..2 * SIZE)
        .step_by(97)
        .chain(SIZE - RESERVED_SIZE..SIZE - 4)
    {
        let mut damaged = slot.clone();
        damaged[at] ^= 0x10;
        let decoded = decode(&profile, 2, &damaged).unwrap();
        assert_eq!(decoded.payload, original, "byte {at}");
        assert!(decoded.needs_repair, "byte {at}");
    }
}

#[test]
fn losing_both_copies_loses_the_slot() {
    let profile = profile(Kind::Main);
    let slot = encode(&profile, 2, &page(), SIZE).unwrap();
    let mut damaged = slot.clone();
    damaged[100] ^= 1;
    damaged[SIZE + 200] ^= 1;
    assert!(decode(&profile, 2, &damaged).unwrap_err().unreadable());
    // The same damage in both: comparing them shows nothing, and only the checksum can.
    let mut same = slot;
    same[100] ^= 1;
    same[SIZE + 100] ^= 1;
    assert!(decode(&profile, 2, &same).unwrap_err().unreadable());
}

#[test]
fn a_copy_proves_its_place_as_well_as_its_bytes() {
    let profile = profile(Kind::Wal);
    let slot = encode(&profile, 5, &frame(), SIZE + 24).unwrap();
    assert!(decode(&profile, 6, &slot).is_err());
    let mut other = profile;
    other.id[0] ^= 1;
    assert!(decode(&other, 5, &slot).is_err());
}

#[test]
fn copies_from_two_different_writes_are_damage_and_not_a_choice() {
    for (kind, original) in each_kind() {
        let profile = profile(kind);
        let mut changed = original.clone();
        changed[100] ^= 1;
        let old = encode(&profile, 2, &original, original.len()).unwrap();
        let new = encode(&profile, 2, &changed, changed.len()).unwrap();
        for (first, second) in [(&old, &new), (&new, &old)] {
            let mut mixed = first[..SIZE].to_vec();
            mixed.extend_from_slice(&second[SIZE..]);
            assert!(decode(&profile, 2, &mixed).unwrap_err().unreadable());
        }
    }
}

#[test]
fn the_frame_header_is_protected_like_the_page() {
    let profile = profile(Kind::Wal);
    let original = frame();
    let old = encode(&profile, 1, &original, original.len()).unwrap();
    let mut other = original.clone();
    other[5] ^= 1;
    let new = encode(&profile, 1, &other, other.len()).unwrap();
    assert_ne!(old[..SIZE - 4], new[..SIZE - 4]);
    let mut mixed = new.clone();
    mixed[SIZE..].copy_from_slice(&old[SIZE..]);
    assert!(decode(&profile, 1, &mixed).is_err());
}

#[test]
fn a_wrong_copy_number_or_spare_byte_costs_only_a_repair() {
    let profile = profile(Kind::Main);
    let original = page();
    let slot = encode(&profile, 3, &original, SIZE).unwrap();
    for at in [SIZE - 4, SIZE - 1, 2 * SIZE - 4, 2 * SIZE - 1] {
        let mut damaged = slot.clone();
        damaged[at] ^= 0x41;
        let decoded = decode(&profile, 3, &damaged).unwrap();
        assert_eq!(decoded.payload, original);
        assert!(decoded.needs_repair);
        assert!(!decoded.rebuilt);
    }
}

#[test]
fn a_hole_is_both_copies_zero() {
    let profile = profile(Kind::Main);
    let decoded = decode(&profile, 0, &vec![0; 2 * SIZE]).unwrap();
    assert!(decoded.zero);
    assert!(!decoded.needs_repair);
    assert!(decoded.payload.iter().all(|&byte| byte == 0));
    let mut bad = vec![0; 2 * SIZE];
    bad[SIZE + 7] = 1;
    assert!(decode(&profile, 0, &bad).is_err());
}

#[test]
fn a_slot_shorter_than_both_copies_is_not_a_slot() {
    let profile = profile(Kind::Wal);
    let slot = encode(&profile, 0, &frame(), SIZE + 24).unwrap();
    for length in [0, 1, SIZE, SIZE + 1, 2 * SIZE - 1] {
        assert!(decode(&profile, 0, &slot[..length]).is_err());
    }
    assert!(decode(&profile, 0, &vec![0; 2 * SIZE + 1]).is_err());
}

#[test]
fn a_partial_payload_keeps_its_initialized_length() {
    for (kind, original) in each_kind() {
        let profile = profile(kind);
        for length in [1, 23, 24, 25, 100, SIZE - RESERVED_SIZE] {
            let mut input = original.clone();
            input[length..].fill(0);
            let slot = encode(&profile, 1, &input[..length], length).unwrap();
            let decoded = decode(&profile, 1, &slot).unwrap();
            assert_eq!(decoded.initialized, length);
            assert_eq!(decoded.payload, input);
        }
    }
}

#[test]
fn a_record_survives_damage_to_one_of_its_two_copies() {
    let data = [0x3c; HEADER_DATA_SIZE];
    let record = protect(&data).unwrap();
    assert_eq!(record.len(), HEADER_RECORD_SIZE);
    assert_eq!(decode_copies(&record), [Some(data), Some(data)]);
    for at in [
        0,
        127,
        128,
        143,
        HEADER_RECORD_SIZE / 2,
        HEADER_RECORD_SIZE / 2 + 143,
    ] {
        let mut damaged = record.clone();
        damaged[at] ^= 1;
        let expected = if at < HEADER_RECORD_SIZE / 2 {
            [None, Some(data)]
        } else {
            [Some(data), None]
        };
        assert_eq!(decode_copies(&damaged), expected, "byte {at}");
    }
    let mut both = record;
    both[5] ^= 1;
    both[HEADER_RECORD_SIZE / 2 + 5] ^= 1;
    assert_eq!(decode_copies(&both), [None, None]);
}

#[test]
fn the_other_builds_file_is_refused_by_name() {
    let mut prefix = encode_prefix(&profile(Kind::Main)).unwrap();
    assert!(decode_prefix(&prefix).is_ok());
    prefix[..8].copy_from_slice(&OTHER_MAGIC);
    let error = decode_prefix(&prefix).unwrap_err();
    assert!(error.message.contains("other MonsterVfs build"));
}

#[test]
fn the_defaults_have_no_parity() {
    assert_eq!(Settings::default().parity_bytes, 0);
    assert_eq!(Settings::default(), Settings::new(0).unwrap());
    for parity in [1, 32, 1024, usize::MAX] {
        assert!(Settings::new(parity).is_err());
    }
}
