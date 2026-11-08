use super::*;

fn profile(settings: Settings, kind: Kind) -> Profile {
    Profile::new(settings, kind, [7; 16]).unwrap()
}

#[cfg(feature = "copy")]
#[test]
fn copied_slots_are_aligned_and_have_whole_slot_eof() {
    for kind in [Kind::Main, Kind::Wal] {
        let profile = profile(Settings::new(0).unwrap(), kind);
        assert_eq!(profile.slot_size(), 8192);
        assert_eq!(HEADER_SIZE % 4096, 0);
        assert_eq!(profile.content_start() % 4096, 0);
        let base = profile.logical_base();
        let payload = profile.payload_size() as u64;
        for length in [
            0,
            base + payload,
            base + 13 * payload,
            profile.max_logical_length(),
        ] {
            let physical = profile.physical_length(length).unwrap();
            assert!(physical <= i64::MAX as u64);
            assert_eq!(profile.logical_length(physical).unwrap(), length);
        }
        // A partial slot takes a whole one, so that both copies exist.
        for (length, slots) in [(base + 1, 1), (base + payload + 1, 2)] {
            let physical = profile.physical_length(length).unwrap();
            assert_eq!(physical, profile.slot_offset(slots).unwrap());
        }
    }
}

#[cfg(feature = "copy")]
#[test]
fn a_tail_shorter_than_a_slot_holds_no_logical_bytes() {
    for kind in [Kind::Main, Kind::Wal] {
        let profile = profile(Settings::default(), kind);
        let whole = profile.slot_offset(3).unwrap();
        let length = profile.logical_length(whole).unwrap();
        for tail in [1, 4095, 4096, 4097, 8191] {
            assert_eq!(profile.logical_length(whole + tail).unwrap(), length);
        }
    }
}

#[cfg(feature = "parity")]
#[test]
fn every_parity_configuration_has_aligned_slots_and_round_trips_eof() {
    for parity in (32..=1024).step_by(2) {
        for kind in [Kind::Main, Kind::Wal, Kind::Temporary] {
            let profile = profile(Settings::new(parity).unwrap(), kind);
            assert_eq!(profile.slot_size(), 8192);
            assert_eq!(HEADER_SIZE % 4096, 0);
            assert_eq!(profile.content_start() % 4096, 0);
            assert!(profile.prefix_size() >= METADATA_SIZE + HASH_COPIES_SIZE);
            let (data, hashes) = profile.hash_offsets();
            if kind != Kind::Temporary {
                let parity_end = PARITY_OFFSET + profile.parity_bytes();
                assert_eq!(data[0] + HASH_SIZE, hashes[0]);
                assert_eq!(hashes[0] + HASH_SIZE, PARITY_OFFSET);
                assert!(data[1] >= parity_end);
                assert!(hashes[1] + HASH_SIZE <= data[2]);
                assert_eq!(hashes[2] + HASH_SIZE, profile.prefix_size());
            }
            let base = profile.logical_base();
            let payload = profile.payload_size() as u64;
            for length in [
                0,
                base + 1,
                base + payload - 1,
                base + payload,
                base + payload + 1,
                base + 13 * payload + 47,
                profile.max_logical_length(),
            ] {
                let physical = profile.physical_length(length).unwrap();
                assert!(physical <= i64::MAX as u64);
                assert_eq!(profile.logical_length(physical).unwrap(), length);
            }
            assert!(
                profile
                    .physical_length(profile.max_logical_length() + 1)
                    .is_err()
            );
            assert!(profile.slot_offset(profile.max_blocks() + 1).is_err());
        }
    }
}

#[test]
fn malformed_lengths_and_offsets_are_rejected() {
    let profile = profile(Settings::default(), Kind::Main);
    for physical in [0, HEADER_SIZE as u64 - 1, u64::MAX] {
        assert!(profile.logical_length(physical).is_err());
    }
    assert!(profile.block_offset(u64::MAX).is_err());
    assert!(profile.slot_offset(u64::MAX).is_err());
    assert!(profile.physical_length(u64::MAX).is_err());
    assert_eq!(profile.physical_length(0).unwrap(), 8192);
    assert_eq!(profile.slot_size(), 8192);
    assert_eq!(profile.slot_offset(2).unwrap(), 8192 + 2 * 8192);
}

#[cfg(feature = "parity")]
#[test]
fn a_tail_shorter_than_the_slot_prefix_holds_no_logical_bytes() {
    let profile = profile(Settings::default(), Kind::Main);
    let whole = profile.slot_offset(3).unwrap();
    for tail in [0, 1, profile.prefix_size() as u64] {
        assert_eq!(profile.logical_length(whole + tail).unwrap(), 3 * 4096);
    }
    assert_eq!(
        profile
            .logical_length(whole + profile.prefix_size() as u64 + 9)
            .unwrap(),
        3 * 4096 + 9
    );
}

#[test]
fn each_wal_frame_owns_one_slot_after_the_control_region() {
    let wal = profile(Settings::default(), Kind::Wal);
    assert_eq!(wal.content_start(), 16384);
    assert_eq!(wal.payload_size(), 4096 + 24);
    assert_eq!(wal.slot_size(), 8192);
    assert_eq!(wal.slot_offset(2).unwrap(), 16384 + 2 * 8192);
    assert_eq!(wal.logical_length(16384).unwrap(), 0);
    assert_eq!(
        wal.logical_length(wal.slot_offset(2).unwrap()).unwrap(),
        32 + 2 * 4120
    );
    assert_eq!(
        wal.physical_length(32 + 2 * 4120).unwrap(),
        wal.slot_offset(2).unwrap()
    );
    assert_eq!(wal.physical_length(0).unwrap(), 16384);
}

#[test]
fn sparse_offsets_require_no_intermediate_records() {
    let profile = profile(Settings::default(), Kind::Temporary);
    let offset = 1u64 << 40;
    assert_eq!(profile.block_offset(offset / 4096).unwrap(), offset);
    assert_eq!(
        profile
            .logical_length(profile.physical_length(offset + 23).unwrap())
            .unwrap(),
        offset + 23
    );
}
