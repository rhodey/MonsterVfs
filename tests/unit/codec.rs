use super::*;

fn profile(kind: Kind) -> Profile {
    Profile::new(Settings::default(), kind, [0x35; 16]).unwrap()
}

fn payload() -> Vec<u8> {
    let mut page: Vec<u8> = (0..4096).map(|i| (i * 17 + i / 31) as u8).collect();
    page[4096 - RESERVED_SIZE..].fill(0);
    page
}

#[test]
fn canonical_xxh128_bytes_and_parts() {
    assert_eq!(
        hash(&[b""]),
        [
            0x99, 0xaa, 0x06, 0xd3, 0x01, 0x47, 0x98, 0xd8, 0x60, 0x01, 0xc3, 0x24, 0x46, 0x8d,
            0x49, 0x7f,
        ]
    );
    assert_eq!(hash(&[b"abc", b"def"]), hash(&[b"abcdef"]));
}

#[test]
fn ordinary_and_temporary_slots_round_trip_exact_payloads() {
    for kind in [Kind::Main, Kind::Wal, Kind::Temporary] {
        let profile = profile(kind);
        for len in [0, 1, 99, 100, 511, 512, 4095, 4096] {
            let input = payload()[..len].to_vec();
            let slot = encode_slot(&profile, 7, &input, len).unwrap();
            let decoded = decode_slot(&profile, 7, &slot).unwrap();
            assert_eq!(&decoded.payload[..len], &input);
            assert!(decoded.payload[len..].iter().all(|&b| b == 0));
            assert_eq!(decoded.initialized, len);
            assert!(!decoded.needs_repair);
            assert!(!decoded.zero);
            if len != 0 {
                let short = &slot[..profile.stored_length(len)];
                let decoded = decode_slot(&profile, 7, short).unwrap();
                assert_eq!(&decoded.payload[..len], &input);
                assert!(!decoded.needs_repair);
            }
        }
    }
}

#[test]
fn a_temporary_slot_has_no_second_chance() {
    let profile = profile(Kind::Temporary);
    let mut slot = encode_slot(&profile, 0, &payload(), 4096).unwrap();
    slot[profile.prefix_size()] ^= 1;
    assert!(decode_slot(&profile, 0, &slot).is_err());
}

#[test]
fn reserved_page_bytes_must_be_zero_except_in_temporary_files() {
    let mut page = payload();
    page[4095] = 1;
    for kind in [Kind::Main, Kind::Wal] {
        let profile = profile(kind);
        let mut input = page.clone();
        if kind == Kind::Wal {
            input.splice(0..0, [0; WAL_FRAME_HEADER_SIZE]);
        }
        let length = input.len();
        let error = encode_slot(&profile, 0, &input, length).unwrap_err();
        assert_eq!(error.code, MISUSE);
    }
    let temporary = profile(Kind::Temporary);
    let slot = encode_slot(&temporary, 0, &page, 4096).unwrap();
    assert_eq!(decode_slot(&temporary, 0, &slot).unwrap().payload, page);
}

#[test]
fn invalid_encoding_arguments_do_not_discard_nonzero_bytes() {
    let profile = profile(Kind::Main);
    assert!(encode_slot(&profile, 0, &[1], 0).is_err());
    assert!(encode_slot(&profile, 0, &[1], 2).is_err());
    assert!(encode_slot(&profile, 0, &vec![0; 4097], 4097).is_err());
    assert!(encode_slot(&profile, u64::MAX, &[1], 1).is_err());
    assert!(decode_slot(&profile, 0, &vec![0; profile.slot_size() + 1]).is_err());
}

#[test]
fn prefix_copies_recover_independently_and_reject_conflicts() {
    let profile = profile(Kind::Main);
    let original = encode_prefix(&profile).unwrap();
    let decoded = decode_prefix(&original).unwrap();
    assert_eq!(decoded.profile, profile);
    assert_eq!(decoded.repair, [false, false]);

    let mut damaged = original.clone();
    damaged[16] ^= 1;
    damaged[HEADER_REGION_SIZE + 32] ^= 2;
    let decoded = decode_prefix(&damaged).unwrap();
    assert_eq!(decoded.profile, profile);
    assert_eq!(decoded.repair, [true, true]);

    damaged[..HEADER_RECORD_SIZE].fill(0);
    let decoded = decode_prefix(&damaged).unwrap();
    assert_eq!(decoded.profile, profile);
    assert_eq!(decoded.repair, [true, true]);

    let mut other = profile;
    other.id[0] ^= 1;
    let second = encode_prefix(&other).unwrap();
    let mut conflict = original;
    conflict[HEADER_REGION_SIZE..].copy_from_slice(&second[HEADER_REGION_SIZE..]);
    assert!(decode_prefix(&conflict).is_err());
    assert!(decode_prefix(&vec![0; HEADER_SIZE]).is_err());
    assert!(decode_prefix(&conflict[..HEADER_SIZE - 1]).is_err());
}

#[test]
fn validly_checksummed_unknown_headers_are_not_accepted() {
    let profile = profile(Kind::Main);
    let prefix = encode_prefix(&profile).unwrap();
    for at in [0, 8, 12, 32, 36, 40, 44, 48] {
        let mut data = prefix[..HEADER_DATA_SIZE].to_vec();
        data[at..at + 4].fill(0xff);
        let record = build::protect(&data).unwrap();
        let mut invalid = prefix.clone();
        for offset in [0, HEADER_REGION_SIZE] {
            invalid[offset..offset + HEADER_RECORD_SIZE].copy_from_slice(&record);
        }
        assert!(decode_prefix(&invalid).is_err(), "header field {at}");
    }
}

#[test]
fn verified_unsupported_geometry_is_not_an_unfinished_creation() {
    use crate::file::Mapped;
    use crate::test_io::Io;

    for kind in [Kind::Main, Kind::Wal, Kind::Temporary] {
        let prefix = encode_prefix(&profile(kind)).unwrap();
        assert_eq!(&prefix[32..36], &4096u32.to_le_bytes());
        assert_eq!(&prefix[40..44], &4096u32.to_le_bytes());
        for (at, value) in [
            (32, 1024u32),
            (32, 8192),
            (32, 65536),
            (40, 512),
            (40, 8192),
        ] {
            let mut data = prefix[..HEADER_DATA_SIZE].to_vec();
            data[at..at + 4].copy_from_slice(&value.to_le_bytes());
            let record = build::protect(&data).unwrap();
            let mut unsupported = prefix.clone();
            for offset in [0, HEADER_REGION_SIZE] {
                unsupported[offset..offset + HEADER_RECORD_SIZE].copy_from_slice(&record);
            }
            assert_eq!(decode_prefix(&unsupported).unwrap_err().code, CANTOPEN);
            let mut io = Io::default();
            io.visible.write(0, &unsupported);
            assert_eq!(
                Mapped::unfinished(&mut io, kind).unwrap_err().code,
                CANTOPEN
            );
            assert_eq!(io.write_count(), 0);
            assert_eq!(Mapped::open(&mut io).err().unwrap().code, CANTOPEN);
        }
    }
}

#[test]
fn control_records_round_trip_and_correct_small_damage() {
    let control = Control {
        sequence: 7,
        wal_header: [3; 32],
        backfill_salts: [4; 8],
        backfill_frames: 99,
    };
    let id = profile(Kind::Wal).id;
    let record = encode_control(&id, &control).unwrap();
    assert_eq!(&record[60..76], &id);
    assert_eq!(record.len(), HEADER_RECORD_SIZE);
    let candidates = decode_controls(&id, &record).unwrap();
    assert!(candidates.iter().flatten().next().is_some());
    assert!(candidates.iter().flatten().all(|item| *item == control));
    let mut damaged = record.clone();
    damaged[20] ^= 0x10;
    damaged[60] ^= 0x01;
    let candidates = decode_controls(&id, &damaged).unwrap();
    assert!(candidates.iter().flatten().next().is_some());
    assert!(candidates.iter().flatten().all(|item| *item == control));
}

#[test]
fn unverifiable_control_records_are_not_candidates() {
    let id = profile(Kind::Wal).id;
    assert!(
        decode_controls(&id, &[0; HEADER_RECORD_SIZE])
            .unwrap()
            .iter()
            .all(Option::is_none)
    );
    assert!(
        decode_controls(&id, &[0x5a; HEADER_RECORD_SIZE])
            .unwrap()
            .iter()
            .all(Option::is_none)
    );
    let prefix = encode_prefix(&profile(Kind::Wal)).unwrap();
    assert!(
        decode_controls(&id, &prefix[..HEADER_RECORD_SIZE])
            .unwrap()
            .iter()
            .all(Option::is_none)
    );
}

#[cfg(feature = "copy")]
fn header_halves(values: [Option<u8>; 4]) -> Vec<u8> {
    let mut bytes = vec![0; HEADER_SIZE];
    for (index, value) in values.into_iter().enumerate() {
        if let Some(value) = value {
            let mut candidate = profile(Kind::Main);
            candidate.id[0] ^= value;
            let encoded = encode_prefix(&candidate).unwrap();
            let offset = index * (HEADER_RECORD_SIZE / 2);
            bytes[offset..offset + HEADER_RECORD_SIZE / 2]
                .copy_from_slice(&encoded[..HEADER_RECORD_SIZE / 2]);
        }
    }
    bytes
}

#[cfg(feature = "copy")]
#[test]
fn immutable_prefix_majority_is_independent_of_half_position() {
    for foreign in 0..4 {
        let mut values = [Some(0); 4];
        values[foreign] = Some(1);
        let prefix = decode_prefix(&header_halves(values)).unwrap();
        assert_eq!(prefix.profile, profile(Kind::Main));
        let mut repair = [false; 2];
        repair[foreign / 2] = true;
        assert_eq!(prefix.repair, repair);
        assert_eq!(prefix.usable, repair.map(|value| !value));
    }
    // A partial repair of the conflicting region leaves an intact agreeing region.
    for missing in 0..4 {
        for foreign in 0..4 {
            if foreign == missing {
                continue;
            }
            let mut values = [Some(0); 4];
            values[missing] = None;
            values[foreign] = Some(1);
            let decoded = decode_prefix(&header_halves(values));
            if missing / 2 == foreign / 2 {
                assert_eq!(decoded.unwrap().profile, profile(Kind::Main));
            } else {
                // Neither region can stand alone if repairing the other fails.
                assert_eq!(decoded.unwrap_err().code, 11);
            }
        }
    }
}

#[cfg(feature = "copy")]
#[test]
fn unanimous_surviving_prefix_halves_still_recover() {
    for mask in 1u8..16 {
        let values = std::array::from_fn(|index| (mask & (1 << index) != 0).then_some(0));
        let prefix = decode_prefix(&header_halves(values)).unwrap();
        assert_eq!(prefix.profile, profile(Kind::Main));
        assert_eq!(prefix.usable, [mask & 3 != 0, mask & 12 != 0]);
        assert_eq!(prefix.repair, [mask & 3 != 3, mask & 12 != 12]);
    }
}

#[cfg(feature = "copy")]
#[test]
fn ambiguous_immutable_prefixes_are_not_torn_or_missing_records() {
    for values in [
        [Some(0), Some(0), Some(1), Some(1)],
        [Some(0), Some(1), Some(0), Some(1)],
        [Some(0), Some(0), Some(1), Some(2)],
        [Some(0), Some(1), None, None],
        [Some(0), Some(1), Some(2), None],
    ] {
        for shift in 0..4 {
            let mut rotated = values;
            rotated.rotate_left(shift);
            assert_eq!(decode_prefix(&header_halves(rotated)).unwrap_err().code, 11);
        }
    }
}

#[test]
fn control_identity_is_verified_before_it_can_be_used() {
    let id = profile(Kind::Wal).id;
    let mut foreign = id;
    foreign[0] ^= 1;
    let control = Control {
        sequence: 7,
        ..Control::default()
    };
    let record = encode_control(&foreign, &control).unwrap();
    let error = decode_controls(&id, &record).unwrap_err();
    assert_eq!(error.code, CORRUPT);
    assert!(!error.unreadable());

    // Unverified bytes cannot establish a foreign identity. Both copies are lost;
    // parity damage exceeds the record's correction budget.
    let mut damaged = record;
    damaged[..HEADER_DATA_SIZE].fill(0x5a);
    #[cfg(feature = "copy")]
    damaged[HEADER_RECORD_SIZE / 2..HEADER_RECORD_SIZE / 2 + HEADER_DATA_SIZE].fill(0x5a);
    assert!(
        decode_controls(&id, &damaged)
            .unwrap()
            .iter()
            .all(Option::is_none)
    );
}

#[cfg(feature = "copy")]
#[test]
fn an_intact_foreign_control_half_is_not_hidden_by_a_local_half() {
    let id = profile(Kind::Wal).id;
    let control = Control {
        sequence: 7,
        ..Control::default()
    };
    let local = encode_control(&id, &control).unwrap();
    let foreign = encode_control(&[0x12; 16], &control).unwrap();
    let width = HEADER_RECORD_SIZE / 2;
    for half in 0..2 {
        let mut record = local.clone();
        record[half * width..(half + 1) * width].copy_from_slice(&foreign[..width]);
        assert_eq!(decode_controls(&id, &record).unwrap_err().code, CORRUPT);
    }
}
