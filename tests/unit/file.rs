use super::*;
use crate::test_io::{Event, Io, Op, filled, page_bytes};
use crate::{config::Settings, format::Kind};

fn create(kind: Kind) -> (Mapped, Io) {
    let profile = Profile::new(Settings::default(), kind, [0xa5; 16]).unwrap();
    let mut io = Io::default();
    let file = Mapped::create(&mut io, profile).unwrap();
    (file, io)
}

/// Up to 8 bad symbols are correctable. More hit both copies in `copy`, where the same
/// damage to one copy would cost nothing.
fn damage(io: &mut Io, file: &Mapped, block: u64, count: usize) {
    let slot = file.profile.slot_offset(block).unwrap();
    let size = BLOCK_SIZE as u64;
    let mut starts = vec![slot + file.profile.prefix_size() as u64];
    if cfg!(feature = "copy") {
        starts = if count > 8 {
            vec![slot, slot + size]
        } else {
            vec![slot]
        };
    }
    for start in starts {
        for index in 0..count {
            io.visible.flip(start + 2 * index as u64, 0x37);
        }
    }
}

/// The length a file reports after writing or truncating to `length`.
fn stored(file: &Mapped, length: u64) -> u64 {
    let physical = file.profile.physical_length(length).unwrap();
    file.profile.logical_length(physical).unwrap()
}

fn read(file: &Mapped, io: &mut Io, offset: u64, count: usize) -> Vec<u8> {
    let mut bytes = vec![0xff; count];
    assert!(!file.read(io, offset, &mut bytes).unwrap().short);
    bytes
}

#[cfg(feature = "parity")]
#[test]
fn exact_reads_short_reads_and_reopening() {
    let (mut file, mut io) = create(Kind::Main);
    assert_eq!(file.size(&mut io).unwrap(), 0);
    let data = page_bytes((0..9001).map(|n| (n * 23) as u8).collect());
    file.write(&mut io, 0, &data).unwrap();
    assert_eq!(file.size(&mut io).unwrap(), 9001);
    let reopened = Mapped::open(&mut io).unwrap();
    assert_eq!(read(&reopened, &mut io, 0, data.len()), data);
    let mut tail = [0xff; 20];
    let outcome = file.read(&mut io, 8991, &mut tail).unwrap();
    assert!(outcome.short);
    assert!(outcome.observations.is_empty());
    assert_eq!(&tail[..10], &data[8991..]);
    assert_eq!(&tail[10..], &[0; 10]);
    let mut beyond = [1; 16];
    assert!(file.read(&mut io, 99_000, &mut beyond).unwrap().short);
    assert_eq!(beyond, [0; 16]);
}

#[test]
fn complete_replacement_does_not_read_unrecoverable_old_data() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &filled(1, 4096)).unwrap();
    damage(&mut io, &file, 0, 20);
    io.events.clear();
    io.fail(Op::Read, 0, 0, 266);
    file.write(&mut io, 0, &filled(2, 4096)).unwrap();
    assert!(
        io.events
            .iter()
            .all(|event| !matches!(event, Event::Read(..)))
    );
    io.clear_fault();
    assert_eq!(read(&file, &mut io, 0, 4096), filled(2, 4096));
}

#[test]
fn partial_writes_verify_and_repair_preserved_bytes() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &filled(3, 4096)).unwrap();
    damage(&mut io, &file, 0, 8);
    file.write(&mut io, 100, &[4; 17]).unwrap();
    let mut expected = filled(3, 4096);
    expected[100..117].fill(4);
    assert_eq!(read(&file, &mut io, 0, 4096), expected);
    damage(&mut io, &file, 0, 20);
    let writes = io.write_count();
    assert_eq!(file.write(&mut io, 200, &[9]).unwrap_err().code, 8202);
    assert_eq!(io.write_count(), writes);
}

#[test]
fn sparse_gaps_do_not_allocate_or_visit_each_missing_slot() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, b"first").unwrap();
    io.events.clear();
    let far = (1u64 << 40) + 23;
    file.write(&mut io, far, b"last").unwrap();
    assert_eq!(file.size(&mut io).unwrap(), stored(&file, far + 4));
    assert!(io.visible.pages.len() < 40);
    assert_eq!(io.write_count(), 1);
    assert_eq!(
        read(&file, &mut io, far - 23, 27),
        [vec![0; 23], b"last".to_vec()].concat()
    );
    assert_eq!(read(&file, &mut io, 4096 * 101, 4096), vec![0; 4096]);
    assert_eq!(
        read(&file, &mut io, 0, 10),
        [b"first".to_vec(), vec![0; 5]].concat()
    );
}

#[test]
fn truncate_and_extension_cannot_resurrect_bytes() {
    for kind in [Kind::Main, Kind::Temporary] {
        let (mut file, mut io) = create(kind);
        file.write(&mut io, 0, &filled(8, 9000)).unwrap();
        file.truncate(&mut io, 4113).unwrap();
        assert_eq!(file.size(&mut io).unwrap(), stored(&file, 4113));
        file.truncate(&mut io, 12_000).unwrap();
        assert_eq!(
            read(&file, &mut io, 4096, 40),
            [vec![8; 17], vec![0; 23]].concat()
        );
        assert_eq!(read(&file, &mut io, 8192, 100), vec![0; 100]);
        file.truncate(&mut io, 0).unwrap();
        assert_eq!(io.visible.length, HEADER_SIZE as u64);
        let reopened = Mapped::open(&mut io).unwrap();
        assert_eq!(reopened.size(&mut io).unwrap(), 0);
    }
}

#[test]
fn partial_truncation_rewrites_then_truncates_and_preserves_errors() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &filled(7, 4096)).unwrap();
    io.events.clear();
    io.fail(Op::Truncate, 0, 0, 1546);
    let error = file.truncate(&mut io, 4040).unwrap_err();
    assert_eq!(error.code, 1546);
    let modifications: Vec<_> = io
        .events
        .iter()
        .filter(|event| matches!(event, Event::Write(..) | Event::Truncate(..)))
        .collect();
    assert!(matches!(
        modifications.as_slice(),
        [Event::Write(_, 8192), Event::Truncate(_)]
    ));
    assert_eq!(file.size(&mut io).unwrap(), 4096);
    assert_eq!(
        read(&file, &mut io, 4032, 16),
        [vec![7; 8], vec![0; 8]].concat()
    );

    io.events.clear();
    io.fail(Op::Write, 0, 0, 778);
    assert_eq!(file.truncate(&mut io, 4000).unwrap_err().code, 778);
    assert!(
        !io.events
            .iter()
            .any(|event| matches!(event, Event::Truncate(..)))
    );
}

#[cfg(feature = "parity")]
#[test]
fn interruption_images_obey_eof_and_prevent_resurrection() {
    let (mut file, mut old) = create(Kind::Main);
    file.write(&mut old, 0, &filled(6, 4096)).unwrap();
    let old_length = old.visible.length;
    let new_length = file.profile.physical_length(4040).unwrap();
    let mut new = old.clone();
    file.truncate(&mut new, 4040).unwrap();

    let mut new_record_old_eof = new.clone();
    new_record_old_eof.visible.truncate(old_length);
    assert_eq!(
        read(&file, &mut new_record_old_eof, 4032, 16),
        [vec![6; 8], vec![0; 8]].concat()
    );

    let mut old_record_new_eof = old.clone();
    old_record_new_eof.visible.truncate(new_length);
    assert_eq!(read(&file, &mut old_record_new_eof, 0, 4040), vec![6; 4040]);
    file.write(&mut old_record_new_eof, 4047, &[9]).unwrap();
    assert_eq!(
        read(&file, &mut old_record_new_eof, 4040, 8),
        [vec![0; 7], vec![9]].concat()
    );
    assert_eq!(read(&file, &mut new, 0, 4040), vec![6; 4040]);
    assert_eq!(read(&file, &mut old, 0, 4096), filled(6, 4096));
}

#[test]
fn native_read_and_size_errors_never_become_zero_holes() {
    let (mut file, mut io) = create(Kind::Main);
    file.truncate(&mut io, 4096).unwrap();
    for code in [266, 522] {
        io.fail(Op::Read, 0, 3, code);
        assert_eq!(file.read(&mut io, 0, &mut [0; 16]).unwrap_err().code, code);
    }
    io.fail(Op::Size, 0, 0, 1802);
    assert_eq!(file.size(&mut io).unwrap_err().code, 1802);
}

#[test]
fn sync_distinguishes_durable_contents_from_visible_contents() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, b"old").unwrap();
    io.sync(3).unwrap();
    file.write(&mut io, 0, b"new").unwrap();
    assert_eq!(read(&file, &mut io, 0, 3), b"new");
    let mut crashed = io.crash();
    assert_eq!(read(&file, &mut crashed, 0, 3), b"old");
    io.fail(Op::Sync, 0, 0, 1034);
    assert_eq!(io.sync(3).unwrap_err().code, 1034);
    assert_eq!(read(&file, &mut io.crash(), 0, 3), b"old");
    io.fail(Op::Sync, 0, 1, 1034);
    assert!(io.sync(3).is_err());
    assert_eq!(read(&file, &mut io.crash(), 0, 3), b"new");
}

#[test]
fn prefix_repair_restores_unusable_copy_before_touching_last_usable_copy() {
    for survivor in [0usize, 1] {
        let (mut file, mut io) = create(Kind::Main);
        let missing = 1 - survivor;
        io.visible.flip((survivor * 4096 + 16) as u64, 1);
        io.visible
            .write((missing * 4096) as u64, &[0; HEADER_RECORD_SIZE]);
        io.durable = io.visible.clone();
        io.events.clear();
        assert!(file.repair_prefix(&mut io).unwrap());
        let operations: Vec<_> = io
            .events
            .iter()
            .filter(|event| matches!(event, Event::Write(..) | Event::Sync(..)))
            .cloned()
            .collect();
        assert_eq!(
            operations,
            vec![
                Event::Write((missing * 4096) as u64, HEADER_RECORD_SIZE),
                Event::Sync(3),
                Event::Write((survivor * 4096) as u64, HEADER_RECORD_SIZE),
                Event::Sync(3)
            ]
        );
        assert_eq!(
            Mapped::open(&mut io.crash()).unwrap().prefix_repair,
            [false; 2]
        );
    }
}

#[test]
fn failed_prefix_write_or_sync_preserves_recoverable_old_copy() {
    for fail in [Op::Write, Op::Sync] {
        for partial in [0, 1] {
            let (mut file, mut io) = create(Kind::Main);
            io.visible.flip(16, 1);
            io.visible.write(4096, &[0; HEADER_RECORD_SIZE]);
            io.durable = io.visible.clone();
            io.events.clear();
            io.fail(fail, 0, partial, if fail == Op::Write { 778 } else { 1034 });
            assert!(file.repair_prefix(&mut io).is_err());
            let writes: Vec<_> = io
                .events
                .iter()
                .filter(|event| matches!(event, Event::Write(..)))
                .collect();
            assert_eq!(writes, vec![&Event::Write(4096, HEADER_RECORD_SIZE)]);
            assert_eq!(Mapped::open(&mut io.crash()).unwrap().profile, file.profile);
        }
    }
}

#[test]
fn healthy_prefix_maintenance_does_not_write_or_sync() {
    let (mut file, mut io) = create(Kind::Main);
    io.events.clear();
    assert!(!file.repair_prefix(&mut io).unwrap());
    assert!(
        !io.events
            .iter()
            .any(|event| matches!(event, Event::Write(..) | Event::Sync(..)))
    );
    let mut other = file.profile;
    other.id[0] ^= 1;
    io.visible.write(0, &codec::encode_prefix(&other).unwrap());
    assert!(file.repair_prefix(&mut io).is_err());
}

#[test]
fn write_reports_replaced_blocks_without_per_block_allocations() {
    let (mut file, mut io) = create(Kind::Temporary);
    assert_eq!(file.write(&mut io, u64::MAX, &[]).unwrap(), 0..0);
    assert_eq!(file.write(&mut io, 4095, &[7; 8194]).unwrap(), 0..4);
    assert_eq!(file.write(&mut io, 8192, &filled(8, 4096)).unwrap(), 2..3);
}

#[test]
fn large_reads_bound_observations_but_still_verify_every_slot() {
    let settings = Settings::default();
    let profile = Profile::new(settings, Kind::Main, [3; 16]).unwrap();
    let mut io = Io::default();
    let mut file = Mapped::create(&mut io, profile).unwrap();
    let count = MAX_READ_OBSERVATIONS + 2;
    let mut data = vec![7; count * BLOCK_SIZE];
    for page in data.chunks_mut(BLOCK_SIZE) {
        page[BLOCK_SIZE - RESERVED_SIZE..].fill(0);
    }
    file.write(&mut io, 0, &data).unwrap();
    for block in 0..count {
        // Damaging parity leaves data verifiable, so this tests observation bounds
        // without spending the test's time reconstructing a thousand codewords.
        io.visible
            .flip(profile.slot_offset(block as u64).unwrap() + 80, 1);
    }
    let mut output = vec![0; data.len()];
    let outcome = file.read(&mut io, 0, &mut output).unwrap();
    assert!(!outcome.short);
    assert_eq!(output, data);
    assert_eq!(outcome.observations.len(), MAX_READ_OBSERVATIONS);
    assert_eq!(
        outcome.observations.last().unwrap().block,
        (MAX_READ_OBSERVATIONS - 1) as u64
    );
    let last = (count - 1) as u64;
    assert_eq!(
        file.read(&mut io, last * BLOCK_SIZE as u64, &mut [0; BLOCK_SIZE])
            .unwrap()
            .observations[0]
            .block,
        last
    );
    damage(&mut io, &file, last, 20);
    assert_eq!(file.read(&mut io, 0, &mut output).unwrap_err().code, 8202);
}

fn sqlite_page(size: usize) -> Vec<u8> {
    let mut page = vec![0; size];
    page[..16].copy_from_slice(b"SQLite format 3\0");
    let encoded = if size == 65536 { 1 } else { size as u16 };
    page[16..18].copy_from_slice(&encoded.to_be_bytes());
    page[20] = RESERVED_SIZE as u8;
    page
}

#[test]
fn sqlite_page_size_must_be_4096_on_reads_and_writes() {
    let (mut file, mut io) = create(Kind::Main);
    let valid = sqlite_page(4096);
    file.write(&mut io, 0, &valid).unwrap();
    assert_eq!(read(&file, &mut io, 0, 100), valid[..100]);
    let writes = io.write_count();
    assert_eq!(
        file.write(&mut io, 16, &8192u16.to_be_bytes())
            .unwrap_err()
            .code,
        8202
    );
    assert_eq!(io.write_count(), writes);
    assert_eq!(read(&file, &mut io, 0, 100), valid[..100]);

    let mut wrong = valid.clone();
    wrong[16..18].copy_from_slice(&8192u16.to_be_bytes());
    assert_eq!(file.write(&mut io, 0, &wrong).unwrap_err().code, 8202);
    assert_eq!(io.write_count(), writes);
    let encoded = codec::encode_slot(&file.profile, 0, &wrong, 4096).unwrap();
    io.visible
        .write(file.profile.slot_offset(0).unwrap(), &encoded);
    assert_eq!(file.read(&mut io, 0, &mut [0; 100]).unwrap_err().code, 8202);

    io.events.clear();
    io.fail(Op::Read, 0, 0, 266);
    file.write(&mut io, 0, &valid).unwrap();
    assert!(
        io.events
            .iter()
            .all(|event| !matches!(event, Event::Read(..)))
    );
    io.clear_fault();
    assert_eq!(read(&file, &mut io, 0, 100), valid[..100]);
}

#[test]
fn a_database_without_reserved_bytes_is_refused_on_reads_and_writes() {
    let (mut file, mut io) = create(Kind::Main);
    let mut plain = sqlite_page(4096);
    plain[20] = 0;
    assert_eq!(file.write(&mut io, 0, &plain).unwrap_err().code, 8202);
    assert_eq!(io.write_count(), 1);
    let encoded = codec::encode_slot(&file.profile, 0, &plain, 4096).unwrap();
    io.visible
        .write(file.profile.slot_offset(0).unwrap(), &encoded);
    assert_eq!(file.read(&mut io, 0, &mut [0; 100]).unwrap_err().code, 8202);
}

#[test]
fn bytes_in_a_reserved_tail_are_refused_and_nothing_is_written() {
    for kind in [Kind::Main, Kind::Wal] {
        let (mut file, mut io) = create(kind);
        let offset = if kind == Kind::Wal {
            file.write(&mut io, 0, &wal_header(1)).unwrap();
            append_frame(&mut file, &mut io, 0, 3);
            32 + 24
        } else {
            file.write(&mut io, 0, &filled(3, 4096)).unwrap();
            0
        };
        let writes = io.write_count();
        let error = file.write(&mut io, offset + 4095, &[1]).unwrap_err();
        assert_eq!(error.code, crate::error::MISUSE);
        assert_eq!(io.write_count(), writes);
    }
}

#[test]
fn a_bad_metadata_sector_costs_a_repair_and_no_page() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &filled(3, 8192)).unwrap();
    let slot = file.profile.slot_offset(1).unwrap();
    io.bad_sector = Some((slot, 8458));
    let mut page = vec![0; 4096];
    let outcome = file.read(&mut io, 4096, &mut page).unwrap();
    assert_eq!(page, filled(3, 4096));
    assert_eq!(outcome.observations.len(), 1);
    assert_eq!(outcome.observations[0].block, 1);
    assert_eq!(outcome.zero_slots, 0);
}

#[cfg(feature = "copy")]
#[test]
fn a_bad_sector_on_either_copy_costs_a_repair_and_no_page() {
    for at in [0, 4096] {
        let (mut file, mut io) = create(Kind::Main);
        file.write(&mut io, 0, &filled(3, 4096)).unwrap();
        let slot = file.profile.slot_offset(0).unwrap();
        io.bad_sector = Some((slot + at, 8458));
        let mut page = vec![0; 4096];
        let outcome = file.read(&mut io, 0, &mut page).unwrap();
        assert_eq!(page, filled(3, 4096));
        assert_eq!(outcome.observations.len(), 1);
    }
}

#[cfg(feature = "copy")]
#[test]
fn partial_writes_round_up_to_whole_slots() {
    let (mut file, mut io) = create(Kind::Main);
    let data = page_bytes((0..9001).map(|n| (n * 23) as u8).collect());
    file.write(&mut io, 0, &data).unwrap();
    assert_eq!(file.size(&mut io).unwrap(), 12288);
    assert_eq!(io.visible.length, file.profile.slot_offset(3).unwrap());
    let mut expected = data;
    expected.resize(12288, 0);
    assert_eq!(read(&file, &mut io, 0, 12288), expected);
    let (mut wal, mut io) = create(Kind::Wal);
    wal.write(&mut io, 0, &wal_header(1)).unwrap();
    wal.write(&mut io, 32, &[7; 24]).unwrap();
    wal.flush(&mut io).unwrap();
    assert_eq!(wal.size(&mut io).unwrap(), 32 + 4120);
    assert_eq!(read(&wal, &mut io, 32, 24), vec![7; 24]);
}

#[cfg(feature = "parity")]
#[test]
fn a_bad_page_sector_is_still_an_error() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &filled(3, 4096)).unwrap();
    let slot = file.profile.slot_offset(0).unwrap();
    io.bad_sector = Some((slot + 4096, 8458));
    assert_eq!(file.read(&mut io, 0, &mut [0; 100]).unwrap_err().code, 8458);
}

#[test]
fn a_read_error_above_a_zero_page_sector_is_not_a_zero_slot() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &filled(3, 4096)).unwrap();
    let slot = file.profile.slot_offset(0).unwrap();
    io.visible.write(slot + 4096, &[0; 4096]);
    io.bad_sector = Some((slot, 8458));
    assert_eq!(file.read(&mut io, 0, &mut [0; 100]).unwrap_err().code, 8458);
}

#[cfg(feature = "parity")]
#[test]
fn a_partial_slot_has_no_page_sector_to_fall_back_on() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &[3; 100]).unwrap();
    let slot = file.profile.slot_offset(0).unwrap();
    io.bad_sector = Some((slot, 8458));
    assert_eq!(file.read(&mut io, 0, &mut [0; 100]).unwrap_err().code, 8458);
}

#[test]
fn sqlite_header_size_check_is_specific_to_main_block_zero() {
    let mut wrong = sqlite_page(4096);
    wrong[16..18].copy_from_slice(&8192u16.to_be_bytes());
    for kind in [Kind::Main, Kind::Wal, Kind::Temporary] {
        let (mut file, mut io) = create(kind);
        let offset = match kind {
            Kind::Main => 4096,
            Kind::Wal => 32,
            Kind::Temporary => 0,
        };
        file.write(&mut io, offset, &wrong).unwrap();
        assert_eq!(read(&file, &mut io, offset, 4096), wrong);
    }
    for encoded in [1u16, 1024, 8192] {
        let (mut file, mut io) = create(Kind::Main);
        let mut page = sqlite_page(4096);
        page[16..18].copy_from_slice(&encoded.to_be_bytes());
        assert_eq!(file.write(&mut io, 0, &page).unwrap_err().code, 8202);
    }
}

#[cfg(feature = "parity")]
#[test]
fn complete_final_partial_replacement_needs_no_old_read() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &[1; 32]).unwrap();
    damage(&mut io, &file, 0, 12);
    io.events.clear();
    io.fail(Op::Read, 0, 0, 266);
    file.write(&mut io, 0, &[2; 32]).unwrap();
    assert!(
        io.events
            .iter()
            .all(|event| !matches!(event, Event::Read(..)))
    );
    io.clear_fault();
    assert_eq!(read(&file, &mut io, 0, 32), vec![2; 32]);
    assert_eq!(file.size(&mut io).unwrap(), 32);
}

fn wal_header(salt: u8) -> [u8; 32] {
    let mut data = [0; 32];
    data[..4].copy_from_slice(&0x377f0682u32.to_be_bytes());
    data[4..8].copy_from_slice(&3_007_000u32.to_be_bytes());
    data[8..12].copy_from_slice(&4096u32.to_be_bytes());
    data[16..24].fill(salt);
    let word =
        |at: usize, data: &[u8; 32]| u32::from_le_bytes(data[at..at + 4].try_into().unwrap());
    let (mut a, mut b) = (0u32, 0u32);
    for i in (0..24).step_by(8) {
        a = a.wrapping_add(word(i, &data)).wrapping_add(b);
        b = b.wrapping_add(word(i + 4, &data)).wrapping_add(a);
    }
    data[24..28].copy_from_slice(&a.to_be_bytes());
    data[28..32].copy_from_slice(&b.to_be_bytes());
    data
}

fn frame(fill: u8) -> Vec<u8> {
    [vec![fill; 24], filled(fill, 4096)].concat()
}

fn append_frame(file: &mut Mapped, io: &mut Io, frame: u64, fill: u8) {
    let offset = 32 + frame * 4120;
    file.write(io, offset, &[fill; 24]).unwrap();
    file.write(io, offset + 24, &filled(fill, 4096)).unwrap();
}

#[test]
fn one_frame_is_one_full_slot_write_with_no_read_of_the_old_slot() {
    let (mut file, mut io) = create(Kind::Wal);
    file.write(&mut io, 0, &wal_header(1)).unwrap();
    append_frame(&mut file, &mut io, 0, 2);
    append_frame(&mut file, &mut io, 1, 3);
    io.events.clear();
    append_frame(&mut file, &mut io, 2, 4);
    let physical: Vec<_> = io
        .events
        .iter()
        .filter(|event| matches!(event, Event::Read(..) | Event::Write(..)))
        .cloned()
        .collect();
    assert_eq!(
        physical,
        vec![Event::Write(file.profile.slot_offset(2).unwrap(), 8192)]
    );
    assert_eq!(file.size(&mut io).unwrap(), 32 + 3 * 4120);
    assert_eq!(read(&file, &mut io, 32 + 2 * 4120, 4120), frame(4));
}

#[test]
fn a_trim_cuts_a_frame_it_lands_in_whole_and_never_reads_it() {
    let (mut file, mut io) = create(Kind::Wal);
    file.write(&mut io, 0, &wal_header(1)).unwrap();
    for index in 0..4 {
        append_frame(&mut file, &mut io, index, 2 + index as u8);
    }
    damage(&mut io, &file, 2, 20);
    io.events.clear();
    file.truncate(&mut io, 32 + 2 * 4120 + 1000).unwrap();
    let physical: Vec<_> = io
        .events
        .iter()
        .filter(|event| !matches!(event, Event::Size))
        .cloned()
        .collect();
    assert_eq!(
        physical,
        vec![Event::Truncate(file.profile.slot_offset(2).unwrap())]
    );
    assert_eq!(file.size(&mut io).unwrap(), 32 + 2 * 4120);
    assert_eq!(read(&file, &mut io, 32 + 4120, 4120), frame(3));
    append_frame(&mut file, &mut io, 2, 9);
    assert_eq!(read(&file, &mut io, 32 + 2 * 4120, 4120), frame(9));
}

#[test]
fn a_cut_inside_a_damaged_main_page_keeps_failing() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &filled(7, 8192)).unwrap();
    damage(&mut io, &file, 1, 20);
    assert!(file.truncate(&mut io, 4096 + 1000).is_err());
    assert_eq!(file.size(&mut io).unwrap(), 8192);
}

#[test]
fn rot_in_a_dead_frame_cannot_stop_the_writer_that_replaces_it() {
    let (mut file, mut io) = create(Kind::Wal);
    file.write(&mut io, 0, &wal_header(1)).unwrap();
    for frame in 0..3 {
        append_frame(&mut file, &mut io, frame, 5);
    }
    damage(&mut io, &file, 1, 20);
    file.write(&mut io, 0, &wal_header(2)).unwrap();
    append_frame(&mut file, &mut io, 0, 6);
    append_frame(&mut file, &mut io, 1, 7);
    assert_eq!(read(&file, &mut io, 32 + 4120, 4120), frame(7));
}

#[test]
fn lone_frame_header_merges_with_its_verified_frame() {
    let (mut file, mut io) = create(Kind::Wal);
    file.write(&mut io, 0, &wal_header(1)).unwrap();
    append_frame(&mut file, &mut io, 0, 8);
    file.write(&mut io, 32, &[9; 24]).unwrap();
    assert!(file.pending.is_some());
    file.flush(&mut io).unwrap();
    assert!(file.pending.is_none());
    assert_eq!(read(&file, &mut io, 32, 24), vec![9; 24]);
    assert_eq!(read(&file, &mut io, 56, 4096), filled(8, 4096));
}

#[test]
fn wal_header_alternates_records_and_survives_a_torn_replacement() {
    let (mut file, mut io) = create(Kind::Wal);
    file.write(&mut io, 0, &wal_header(1)).unwrap();
    append_frame(&mut file, &mut io, 0, 2);
    assert_eq!(read(&file, &mut io, 0, 32), wal_header(1));
    io.sync(3).unwrap();
    file.write(&mut io, 0, &wal_header(2)).unwrap();
    assert_eq!(read(&file, &mut io, 0, 32), wal_header(2));
    assert_eq!(file.control(&mut io).unwrap().unwrap().sequence, 2);
    let mut crashed = io.crash();
    assert_eq!(read(&file, &mut crashed, 0, 32), wal_header(1));
    // Sequence 2 lives in the first region.
    io.visible
        .write(HEADER_SIZE as u64, &[0x5a; HEADER_RECORD_SIZE]);
    assert_eq!(read(&file, &mut io, 0, 32), wal_header(1));
}

#[test]
fn backfill_records_the_current_generation_and_keeps_the_header() {
    let (mut file, mut io) = create(Kind::Wal);
    file.write(&mut io, 0, &wal_header(7)).unwrap();
    file.record_backfill(&mut io, 11).unwrap();
    let control = file.control(&mut io).unwrap().unwrap();
    assert_eq!(control.backfill_frames, 11);
    assert_eq!(control.backfill_salts, [7; 8]);
    assert_eq!(control.wal_header, wal_header(7));
    file.write(&mut io, 0, &wal_header(8)).unwrap();
    let control = file.control(&mut io).unwrap().unwrap();
    assert_eq!(control.backfill_salts, [7; 8]);
    assert_ne!(control.backfill_salts[..], control.wal_header[16..24]);
}

#[test]
fn a_tail_too_short_to_hold_payload_was_never_durable_and_is_absent() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 0, &filled(3, 4096)).unwrap();
    io.visible.length = file.profile.physical_length(4096).unwrap() + 4096;
    assert_eq!(file.size(&mut io).unwrap(), 4096);
    assert_eq!(read(&file, &mut io, 0, 4096), filled(3, 4096));
    file.write(&mut io, 4096, &filled(4, 4096)).unwrap();
    assert_eq!(read(&file, &mut io, 4096, 4096), filled(4, 4096));
    assert_eq!(file.size(&mut io).unwrap(), 8192);
}

#[test]
fn creation_makes_the_prefix_durable_before_a_wal_can_grow() {
    let profile = Profile::new(Settings::default(), Kind::Wal, [2; 16]).unwrap();
    let mut io = Io::default();
    Mapped::create(&mut io, profile).unwrap();
    assert!(matches!(
        io.events
            .iter()
            .filter(|event| !matches!(event, Event::Size))
            .collect::<Vec<_>>()
            .as_slice(),
        [
            Event::Write(0, 8192),
            Event::Sync(2),
            Event::Truncate(16384)
        ]
    ));
    assert!(Mapped::open(&mut io.crash()).is_ok());
}

#[test]
fn zero_slots_are_counted_for_status() {
    let (mut file, mut io) = create(Kind::Main);
    file.write(&mut io, 4096, &filled(1, 4096)).unwrap();
    let outcome = file.read(&mut io, 0, &mut [0; 4096]).unwrap();
    assert_eq!(outcome.zero_slots, 1);
    assert_eq!(
        file.read(&mut io, 4096, &mut [0; 4096]).unwrap().zero_slots,
        0
    );
}

#[test]
fn a_record_with_an_invalid_header_never_replaces_a_valid_one() {
    let (mut file, mut io) = create(Kind::Wal);
    file.write(&mut io, 0, &wal_header(1)).unwrap();
    file.write(&mut io, 0, &[0xee; 32]).unwrap();
    let control = file.control(&mut io).unwrap().unwrap();
    assert_eq!(control.wal_header, wal_header(1));
}

#[test]
fn bytes_written_across_the_header_boundary_land_on_both_sides() {
    let (mut file, mut io) = create(Kind::Wal);
    file.write(&mut io, 0, &wal_header(1)).unwrap();
    append_frame(&mut file, &mut io, 0, 2);
    let mut update = wal_header(3).to_vec();
    update.extend_from_slice(&[9; 40]);
    file.write(&mut io, 0, &update).unwrap();
    file.flush(&mut io).unwrap();
    assert_eq!(read(&file, &mut io, 0, 32), wal_header(3));
    assert_eq!(read(&file, &mut io, 32, 40), vec![9; 40]);
    assert_eq!(read(&file, &mut io, 72, 8), vec![2; 8]);
}

#[test]
fn a_checkpoint_that_cannot_record_its_watermark_is_refused() {
    let (file, mut io) = create(Kind::Wal);
    let error = file.record_backfill(&mut io, 3).unwrap_err();
    assert_eq!(error.code, crate::error::CORRUPT_BACKFILLED);
    assert_eq!(io.write_count(), 1, "only the prefix was written");
}

#[test]
fn a_file_with_no_room_for_content_and_no_usable_prefix_is_unfinished() {
    let image = |bytes: &[u8]| {
        let mut io = Io::default();
        if !bytes.is_empty() {
            io.write(0, bytes).unwrap();
        }
        io
    };
    let junk: Vec<u8> = (0..8192usize).map(|i| (i * 37 + 11) as u8).collect();
    for kind in [Kind::Main, Kind::Wal] {
        assert!(Mapped::unfinished(&mut image(&[]), kind).unwrap());
        assert!(Mapped::unfinished(&mut image(&[0; 100]), kind).unwrap());
        assert!(Mapped::unfinished(&mut image(&[0; 8192]), kind).unwrap());
        assert!(Mapped::unfinished(&mut image(&junk), kind).unwrap());
        // SQLite's own files are not ours to replace.
        let mut plain = vec![0; 8192];
        plain[..16].copy_from_slice(b"SQLite format 3\0");
        assert!(!Mapped::unfinished(&mut image(&plain), kind).unwrap());
        assert!(!Mapped::unfinished(&mut image(&plain[..4096]), kind).unwrap());
    }
    // Only a WAL has a control region, which can be all that a crash left of it.
    assert!(Mapped::unfinished(&mut image(&[0; 16384]), Kind::Wal).unwrap());
    // With content possible, a lost prefix is real damage, so it must not be replaced.
    assert!(!Mapped::unfinished(&mut image(&[0; 16384]), Kind::Main).unwrap());
    assert!(!Mapped::unfinished(&mut image(&[0; 24576]), Kind::Wal).unwrap());
}

#[test]
fn a_bad_sector_on_one_prefix_copy_costs_only_that_copy() {
    for (skip, expected) in [(0, [true, false]), (1, [false, true])] {
        let (_, mut io) = create(Kind::Main);
        io.fail(Op::Read, skip, 0, crate::error::IOERR_CORRUPTFS);
        assert_eq!(Mapped::open(&mut io).unwrap().prefix_repair, expected);
        io.clear_fault();
    }
}

#[test]
fn a_prefix_with_both_copies_lost_beside_content_stays_an_error() {
    let (_, mut io) = create(Kind::Main);
    io.write(HEADER_SIZE as u64 + 100, &[1; 100]).unwrap();
    for offset in [0, HEADER_REGION_SIZE as u64] {
        io.write(offset, &[0x5a; HEADER_REGION_SIZE]).unwrap();
    }
    assert!(Mapped::open(&mut io).err().unwrap().unreadable());
}

#[cfg(feature = "copy")]
fn replace_control_half(io: &mut Io, region: u64, half: usize, record: &[u8]) {
    let width = HEADER_RECORD_SIZE / 2;
    io.visible.write(
        control_offset(region) + (half * width) as u64,
        &record[..width],
    );
}

#[cfg(feature = "copy")]
#[test]
fn newest_control_half_wins_and_checkpoint_preserves_its_generation() {
    for region in 0..2 {
        for half in 0..2 {
            let (mut file, mut io) = create(Kind::Wal);
            let newest = Control {
                sequence: 10 + region,
                wal_header: wal_header(2),
                backfill_salts: [1; 8],
                backfill_frames: 3,
            };
            let previous = Control {
                sequence: newest.sequence - 1,
                wal_header: wal_header(1),
                ..newest.clone()
            };
            let stale = Control {
                sequence: newest.sequence - 2,
                ..previous.clone()
            };
            io.visible.write(
                control_offset(region),
                &codec::encode_control(&file.profile.id, &stale).unwrap(),
            );
            io.visible.write(
                control_offset(1 - region),
                &codec::encode_control(&file.profile.id, &previous).unwrap(),
            );
            replace_control_half(
                &mut io,
                region,
                half,
                &codec::encode_control(&file.profile.id, &newest).unwrap(),
            );
            assert_eq!(file.control(&mut io).unwrap(), Some(newest.clone()));
            append_frame(&mut file, &mut io, 0, 4);
            assert_eq!(read(&file, &mut io, 0, 32), newest.wal_header);
            file.record_backfill(&mut io, 7).unwrap();
            let updated = file.control(&mut io).unwrap().unwrap();
            assert_eq!(updated.sequence, newest.sequence + 1);
            assert_eq!(updated.wal_header, newest.wal_header);
            assert_eq!(
                (updated.backfill_salts, updated.backfill_frames),
                ([2; 8], 7)
            );
        }
    }
}

#[cfg(feature = "copy")]
#[test]
fn malformed_high_control_half_cannot_hide_an_older_valid_half() {
    for bad_field in [0, 76, 16, 8] {
        for bad_half in 0..2 {
            let (file, mut io) = create(Kind::Wal);
            let good = Control {
                sequence: 8,
                wal_header: wal_header(1),
                ..Control::default()
            };
            let higher = Control {
                sequence: 10,
                ..good.clone()
            };
            let mut data =
                codec::encode_control(&file.profile.id, &higher).unwrap()[..128].to_vec();
            data[bad_field] ^= 0x40;
            let bad = if bad_field == 8 {
                // The sequence itself has no proof when its checksum no longer matches.
                let mut unproven = codec::encode_control(&file.profile.id, &higher).unwrap();
                unproven[bad_field] ^= 0x40;
                unproven
            } else {
                crate::copy::protect(&data).unwrap()
            };
            io.visible.write(
                control_offset(0),
                &codec::encode_control(&file.profile.id, &good).unwrap(),
            );
            replace_control_half(&mut io, 0, bad_half, &bad);
            assert_eq!(file.control(&mut io).unwrap(), Some(good));
        }
    }
}

#[test]
fn equal_selected_control_sequences_with_different_contents_are_refused() {
    let (file, mut io) = create(Kind::Wal);
    let first = Control {
        sequence: 9,
        wal_header: wal_header(1),
        ..Control::default()
    };
    let second = Control {
        backfill_frames: 3,
        backfill_salts: [1; 8],
        ..first.clone()
    };
    for reverse in [false, true] {
        let records = if reverse {
            [&second, &first]
        } else {
            [&first, &second]
        };
        for (region, control) in records.into_iter().enumerate() {
            io.visible.write(
                control_offset(region as u64),
                &codec::encode_control(&file.profile.id, control).unwrap(),
            );
        }
        io.events.clear();
        assert_eq!(file.control(&mut io).unwrap_err().code, 11);
        assert_eq!(file.record_backfill(&mut io, 7).unwrap_err().code, 11);
        assert_eq!(io.write_count(), 0);
    }
}

#[cfg(feature = "copy")]
#[test]
fn equal_sequence_conflict_is_checked_after_newest_candidate_selection() {
    for region in 0..2 {
        let (file, mut io) = create(Kind::Wal);
        let first = Control {
            sequence: 4,
            wal_header: wal_header(1),
            ..Control::default()
        };
        let second = Control {
            backfill_frames: 3,
            ..first.clone()
        };
        io.visible.write(
            control_offset(region),
            &codec::encode_control(&file.profile.id, &first).unwrap(),
        );
        replace_control_half(
            &mut io,
            region,
            1,
            &codec::encode_control(&file.profile.id, &second).unwrap(),
        );
        assert_eq!(file.control(&mut io).unwrap_err().code, 11);
        let newest = Control {
            sequence: 5,
            wal_header: wal_header(2),
            ..Control::default()
        };
        io.visible.write(
            control_offset(1 - region),
            &codec::encode_control(&file.profile.id, &newest).unwrap(),
        );
        assert_eq!(file.control(&mut io).unwrap(), Some(newest));
    }
}

#[cfg(feature = "copy")]
#[test]
fn conflicting_prefix_repair_preserves_its_agreeing_region_on_failure() {
    for region in 0..2 {
        for half in 0..2 {
            for failure in [Op::Write, Op::Sync] {
                for partial in [0, 1, 128, 144, 2048, 4096] {
                    let (mut file, mut io) = create(Kind::Main);
                    let mut foreign = file.profile;
                    foreign.id[0] ^= 1;
                    let encoded = codec::encode_prefix(&foreign).unwrap();
                    let start = (region * HEADER_REGION_SIZE) as u64;
                    io.visible.write(
                        start + (half * (HEADER_RECORD_SIZE / 2)) as u64,
                        &encoded[..HEADER_RECORD_SIZE / 2],
                    );
                    io.durable = io.visible.clone();
                    io.events.clear();
                    io.fail(
                        failure,
                        0,
                        partial,
                        if failure == Op::Write { 778 } else { 1034 },
                    );
                    assert!(file.repair_prefix(&mut io).is_err());
                    assert_eq!(Mapped::open(&mut io).unwrap().profile, file.profile);
                    assert_eq!(Mapped::open(&mut io.crash()).unwrap().profile, file.profile);
                    let mut unreadable = io.clone();
                    unreadable.bad_sector = Some((start, crate::error::IOERR_CORRUPTFS));
                    assert_eq!(Mapped::open(&mut unreadable).unwrap().profile, file.profile);
                    assert!(
                        io.events
                            .iter()
                            .filter_map(|event| match event {
                                Event::Write(offset, _) => Some(*offset),
                                _ => None,
                            })
                            .all(|offset| offset == start)
                    );
                    io.clear_fault();
                    file.repair_prefix(&mut io).unwrap();
                    let repaired = Mapped::open(&mut io).unwrap();
                    assert_eq!(repaired.profile, file.profile);
                    assert_eq!(repaired.prefix_repair, [false; 2]);
                    assert_eq!(Mapped::open(&mut io.crash()).unwrap().profile, file.profile);
                }
            }
        }
    }
}

#[test]
fn ambiguous_prefix_only_files_are_not_reinitialized() {
    let (file, mut io) = create(Kind::Main);
    let mut foreign = file.profile;
    foreign.id[0] ^= 1;
    let encoded = codec::encode_prefix(&foreign).unwrap();
    io.visible
        .write(HEADER_REGION_SIZE as u64, &encoded[HEADER_REGION_SIZE..]);
    io.events.clear();
    let mut bytes = vec![0; HEADER_SIZE];
    io.visible.bytes(0, &mut bytes);
    assert_eq!(codec::decode_prefix(&bytes).unwrap_err().code, 11);
    assert_eq!(Mapped::open(&mut io).err().unwrap().code, 11);
    assert_eq!(
        Mapped::unfinished(&mut io, Kind::Main).unwrap_err().code,
        11
    );
    io.visible.truncate((HEADER_SIZE - 1) as u64);
    assert_eq!(
        Mapped::unfinished(&mut io, Kind::Main).unwrap_err().code,
        11
    );
    assert_eq!(io.write_count(), 0);
}

#[cfg(feature = "copy")]
#[test]
fn short_prefix_conflicts_are_not_discarded_as_incomplete_creation() {
    let (file, io) = create(Kind::Main);
    let mut foreign = file.profile;
    foreign.id[0] ^= 1;
    let encoded = codec::encode_prefix(&foreign).unwrap();
    for size in [
        HEADER_REGION_SIZE,
        HEADER_REGION_SIZE + HEADER_REGION_SIZE / 2,
    ] {
        let mut partial = io.clone();
        partial.visible.truncate(size as u64);
        assert!(Mapped::unfinished(&mut partial, Kind::Main).unwrap());
        partial.visible.write(
            (HEADER_RECORD_SIZE / 2) as u64,
            &encoded[..HEADER_RECORD_SIZE / 2],
        );
        // Leave only the two conflicting halves in the first region.
        partial.visible.write(
            HEADER_REGION_SIZE as u64,
            &vec![0; size - HEADER_REGION_SIZE],
        );
        partial.events.clear();
        assert_eq!(
            Mapped::unfinished(&mut partial, Kind::Main)
                .unwrap_err()
                .code,
            11
        );
        assert_eq!(partial.write_count(), 0);
    }
}

#[test]
fn foreign_controls_refuse_without_writes_regardless_of_sequence_or_position() {
    let width = if cfg!(feature = "copy") {
        HEADER_RECORD_SIZE / 2
    } else {
        HEADER_RECORD_SIZE
    };
    for region in 0..2 {
        for half in 0..HEADER_RECORD_SIZE / width {
            for sequence in [1, 2, 3] {
                let (mut file, mut io) = create(Kind::Wal);
                file.write(&mut io, 0, &wal_header(1)).unwrap();
                append_frame(&mut file, &mut io, 0, 1);
                file.record_backfill(&mut io, 1).unwrap();
                assert_eq!(file.control(&mut io).unwrap().unwrap().sequence, 2);
                let foreign = Control {
                    sequence,
                    wal_header: wal_header(9),
                    ..Control::default()
                };
                let record = codec::encode_control(&[0x77; 16], &foreign).unwrap();
                io.visible.write(
                    control_offset(region) + (half * width) as u64,
                    &record[..width],
                );
                let before = io.visible.clone();
                io.events.clear();
                assert_eq!(file.control(&mut io).unwrap_err().code, CORRUPT);
                assert_eq!(
                    file.read(&mut io, 0, &mut [0; 32]).unwrap_err().code,
                    CORRUPT
                );
                assert_eq!(file.record_backfill(&mut io, 1).unwrap_err().code, CORRUPT);
                assert_eq!(
                    file.write(&mut io, 0, &wal_header(2)).unwrap_err().code,
                    CORRUPT
                );
                assert_eq!(io.visible.length, before.length);
                assert_eq!(io.visible.pages, before.pages);
                assert!(
                    io.events
                        .iter()
                        .all(|event| matches!(event, Event::Read(..) | Event::Size))
                );
            }
        }
    }
}

#[test]
fn copied_wal_keeps_its_identity_and_accepts_later_controls() {
    let (mut file, mut io) = create(Kind::Wal);
    file.write(&mut io, 0, &wal_header(1)).unwrap();
    append_frame(&mut file, &mut io, 0, 3);
    file.record_backfill(&mut io, 1).unwrap();
    let expected = file.control(&mut io).unwrap().unwrap();
    let mut copied = Io::default();
    copied.visible = io.visible.clone();
    let mut reopened = Mapped::open(&mut copied).unwrap();
    assert_eq!(reopened.profile.id, file.profile.id);
    assert_eq!(
        reopened.control(&mut copied).unwrap(),
        Some(expected.clone())
    );
    assert_eq!(read(&reopened, &mut copied, 0, 32), expected.wal_header);
    reopened.write(&mut copied, 0, &wal_header(2)).unwrap();
    let next = reopened.control(&mut copied).unwrap().unwrap();
    assert_eq!(next.sequence, expected.sequence + 1);
    assert_eq!(next.wal_header, wal_header(2));
}
