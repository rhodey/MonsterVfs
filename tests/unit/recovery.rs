use super::*;
use crate::test_io::{Event, Io, Op, filled};
use crate::{
    config::Settings,
    error::{IOERR_CORRUPTFS, IOERR_FSTAT},
    format::{HEADER_RECORD_SIZE, Kind, Profile},
};

const SALTS: [u8; 8] = [0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0];
const NEXT: [u8; 8] = [0x77; 8];
// Where `restarted` leaves its two records: writes alternate, starting at 12288.
const PREVIOUS: u64 = 8192;
const CURRENT: u64 = 12288;

fn header_with(page: usize, big_endian: bool, salts: [u8; 8]) -> [u8; 32] {
    let mut data = [0; 32];
    data[..4].copy_from_slice(
        &(if big_endian {
            0x377f0683u32
        } else {
            0x377f0682u32
        })
        .to_be_bytes(),
    );
    data[4..8].copy_from_slice(&3_007_000u32.to_be_bytes());
    data[8..12].copy_from_slice(&(page as u32).to_be_bytes());
    data[16..24].copy_from_slice(&salts);
    let mut sum = [0u32; 2];
    for i in (0..24).step_by(8) {
        let word = |at| {
            let bytes: [u8; 4] = data[at..at + 4].try_into().unwrap();
            if big_endian {
                u32::from_be_bytes(bytes)
            } else {
                u32::from_le_bytes(bytes)
            }
        };
        sum[0] = sum[0].wrapping_add(word(i)).wrapping_add(sum[1]);
        sum[1] = sum[1].wrapping_add(word(i + 4)).wrapping_add(sum[0]);
    }
    data[24..28].copy_from_slice(&sum[0].to_be_bytes());
    data[28..32].copy_from_slice(&sum[1].to_be_bytes());
    data
}

fn header(page: usize, big_endian: bool) -> [u8; 32] {
    header_with(page, big_endian, SALTS)
}

fn append(file: &mut Mapped, io: &mut Io, frame: u64, salts: [u8; 8], commit: bool) {
    let offset = 32 + frame * 4120;
    let mut frame_header = [0u8; 24];
    frame_header[..4].copy_from_slice(&1u32.to_be_bytes());
    frame_header[4..8].copy_from_slice(&(commit as u32).to_be_bytes());
    frame_header[8..16].copy_from_slice(&salts);
    let header = file.control(io).unwrap().unwrap().wal_header;
    let sum = |at: usize, bytes: &[u8]| u32::from_be_bytes(bytes[at..at + 4].try_into().unwrap());
    let (mut a, mut b) = (sum(24, &header), sum(28, &header));
    let mut prior = [0u8; 24];
    if frame > 0 && file.read(io, offset - 4120, &mut prior).is_ok() {
        a = sum(16, &prior);
        b = sum(20, &prior);
    }
    let page = filled(0x21, 4096);
    for pair in frame_header[..8]
        .chunks_exact(8)
        .chain(page.chunks_exact(8))
    {
        a = a
            .wrapping_add(u32::from_le_bytes(pair[..4].try_into().unwrap()))
            .wrapping_add(b);
        b = b
            .wrapping_add(u32::from_le_bytes(pair[4..].try_into().unwrap()))
            .wrapping_add(a);
    }
    frame_header[16..20].copy_from_slice(&a.to_be_bytes());
    frame_header[20..24].copy_from_slice(&b.to_be_bytes());
    file.write(io, offset, &frame_header).unwrap();
    file.write(io, offset + 24, &page).unwrap();
}

/// A WAL of committed single-frame transactions.
fn wal(frames: u64) -> (Mapped, Io) {
    let mut io = Io::default();
    let profile = Profile::new(Settings::default(), Kind::Wal, [0xc3; 16]).unwrap();
    let mut file = Mapped::create(&mut io, profile).unwrap();
    if frames != 0 {
        file.write(&mut io, 0, &header(4096, false)).unwrap();
        for frame in 0..frames {
            append(&mut file, &mut io, frame, SALTS, true);
        }
    }
    (file, io)
}

/// Up to 8 bad symbols are correctable. More hit both copies in `copy`, where the same
/// damage to one copy would cost nothing.
fn damage(file: &Mapped, io: &mut Io, block: u64, count: usize) {
    let slot = file.profile.slot_offset(block).unwrap();
    let mut starts = vec![slot + file.profile.prefix_size() as u64];
    if cfg!(feature = "copy") {
        starts = if count > 8 {
            vec![slot, slot + 4096]
        } else {
            vec![slot]
        };
    }
    for start in starts {
        for i in 0..count {
            io.visible.flip(start + 2 * i as u64, 0x6f);
        }
    }
}

fn cutoff(file: &Mapped, io: &mut Io) -> Cutoff {
    scan(file, io).unwrap().cutoff.expect("a cutoff")
}

#[test]
fn empty_and_intact_wals_require_no_cutoff() {
    for frames in [0, 1, 3] {
        let (file, mut io) = wal(frames);
        let scanned = scan(&file, &mut io).unwrap();
        assert_eq!(scanned.cutoff, None);
        assert!(scanned.observations.is_empty());
    }
}

#[test]
fn corrected_frames_remain_in_wal_and_report_maintenance() {
    let (file, mut io) = wal(3);
    damage(&file, &mut io, 1, 8);
    let scanned = scan(&file, &mut io).unwrap();
    assert_eq!(scanned.cutoff, None);
    assert_eq!(scanned.observations.len(), 1);
    assert_eq!(scanned.observations[0].block, 1);
}

#[test]
fn first_unrecoverable_frame_discards_a_healthy_later_suffix_and_reports_it() {
    let (mut file, mut io) = wal(4);
    damage(&file, &mut io, 1, 20);
    let found = cutoff(&file, &mut io);
    let offset = file.profile.slot_offset(1).unwrap();
    assert_eq!(found.offset, offset);
    assert_eq!((found.later_frames, found.lost_commits), (2, 1));
    io.truncate(offset).unwrap();
    io.sync(3).unwrap();
    assert_eq!(file.size(&mut io.crash()).unwrap(), 32 + 4120);
    append(&mut file, &mut io, 1, SALTS, true);
    assert_eq!(scan(&file, &mut io).unwrap().cutoff, None);
}

#[test]
fn stale_generations_are_not_counted_as_discarded_commits() {
    let (mut file, mut io) = wal(4);
    let next = [0x77; 8];
    file.write(&mut io, 0, &header_with(4096, false, next))
        .unwrap();
    append(&mut file, &mut io, 0, next, true);
    append(&mut file, &mut io, 1, next, true);
    damage(&file, &mut io, 2, 20);
    let found = cutoff(&file, &mut io);
    assert_eq!((found.later_frames, found.lost_commits), (0, 0));
}

#[test]
fn the_last_commit_proves_nothing_because_a_torn_tail_looks_the_same() {
    let (file, mut io) = wal(3);
    damage(&file, &mut io, 1, 20);
    let found = cutoff(&file, &mut io);
    assert_eq!((found.later_frames, found.lost_commits), (1, 0));
}

#[test]
fn a_stale_commit_from_an_earlier_crash_is_not_a_finished_commit() {
    let (mut file, mut io) = wal(4);
    // Same generation, but this frame was never written after the commit before it.
    let mut stale = [0u8; 24];
    stale[..4].copy_from_slice(&1u32.to_be_bytes());
    stale[4..8].copy_from_slice(&1u32.to_be_bytes());
    stale[8..16].copy_from_slice(&SALTS);
    file.write(&mut io, 32 + 3 * 4120, &stale).unwrap();
    file.write(&mut io, 32 + 3 * 4120 + 24, &filled(0x21, 4096))
        .unwrap();
    damage(&file, &mut io, 1, 20);
    let found = cutoff(&file, &mut io);
    assert_eq!((found.later_frames, found.lost_commits), (2, 0));
}

#[test]
fn damage_the_checkpoint_may_have_passed_is_refused_without_changing_anything() {
    let (file, mut io) = wal(5);
    file.record_backfill(&mut io, 3).unwrap();
    damage(&file, &mut io, 2, 20);
    io.events.clear();
    assert_eq!(scan(&file, &mut io).err().unwrap().code, CORRUPT_BACKFILLED);
    assert!(
        !io.events
            .iter()
            .any(|event| matches!(event, Event::Write(..) | Event::Truncate(..)))
    );
}

#[test]
fn damage_beyond_the_watermark_is_still_cut() {
    let (file, mut io) = wal(5);
    file.record_backfill(&mut io, 3).unwrap();
    damage(&file, &mut io, 3, 20);
    assert_eq!(
        cutoff(&file, &mut io).offset,
        file.profile.slot_offset(3).unwrap()
    );
}

#[test]
fn a_watermark_from_an_older_generation_does_not_refuse() {
    let (mut file, mut io) = wal(3);
    file.record_backfill(&mut io, 3).unwrap();
    let next = [0x77; 8];
    file.write(&mut io, 0, &header_with(4096, false, next))
        .unwrap();
    append(&mut file, &mut io, 0, next, true);
    append(&mut file, &mut io, 1, next, true);
    damage(&file, &mut io, 1, 20);
    assert_eq!(
        cutoff(&file, &mut io).offset,
        file.profile.slot_offset(1).unwrap()
    );
}

#[test]
fn a_zero_slot_is_damage_because_sqlite_would_stop_there() {
    for (watermark, refused) in [(0, false), (3, true)] {
        let (file, mut io) = wal(4);
        if watermark != 0 {
            file.record_backfill(&mut io, watermark).unwrap();
        }
        let start = file.profile.slot_offset(1).unwrap();
        io.visible.write(start, &[0; 8192]);
        let result = scan(&file, &mut io);
        if refused {
            assert_eq!(result.err().unwrap().code, CORRUPT_BACKFILLED);
        } else {
            assert_eq!(result.unwrap().cutoff.unwrap().offset, start);
        }
    }
}

#[test]
fn frames_without_a_usable_header_cannot_come_from_a_crash_and_are_refused() {
    let (file, mut io) = wal(2);
    for region in 0..2u64 {
        io.visible
            .write(8192 + region * 4096, &[0x5a; HEADER_RECORD_SIZE]);
    }
    assert_eq!(scan(&file, &mut io).err().unwrap().code, CORRUPT_BACKFILLED);
}

#[test]
fn a_header_with_no_frames_needs_no_cutoff() {
    let (mut file, mut io) = wal(0);
    file.write(&mut io, 0, &header(4096, false)).unwrap();
    assert_eq!(scan(&file, &mut io).unwrap().cutoff, None);
    assert_eq!(file.size(&mut io).unwrap(), 0);
}

#[test]
fn incomplete_final_slot_is_cut() {
    let (file, io) = wal(1);
    let offset = file.profile.slot_offset(1).unwrap();
    for tail in [1, file.profile.prefix_size() as u64] {
        let mut broken = io.clone();
        broken.truncate(offset + tail).unwrap();
        assert_eq!(cutoff(&file, &mut broken).offset, offset);
    }
}

#[test]
fn operating_system_errors_are_never_a_cutoff_signal() {
    let (file, mut io) = wal(3);
    for code in [266, 522, 7] {
        io.fail(Op::Read, 3, 4, code);
        assert_eq!(scan(&file, &mut io).err().unwrap().code, code);
    }
    io.fail(Op::Size, 0, 0, IOERR_FSTAT);
    assert_eq!(scan(&file, &mut io).err().unwrap().code, IOERR_FSTAT);
}

#[test]
fn wal_header_validation_accepts_both_checksum_byte_orders() {
    for big in [false, true] {
        validate_header(&header(4096, big)).unwrap();
        for size in [1024, 8192, 65536] {
            assert!(validate_header(&header(size, big)).is_err());
        }
        let mut wrong = header(4096, big);
        wrong[17] ^= 1;
        assert!(validate_header(&wrong).is_err());
    }
}

#[cfg(feature = "parity")]
#[test]
fn a_bad_page_sector_in_a_frame_is_the_same_loss_as_damage() {
    let (file, mut io) = wal(4);
    let frame = file.profile.slot_offset(1).unwrap();
    io.bad_sector = Some((frame + 4096, crate::error::IOERR_CORRUPTFS));
    let found = scan(&file, &mut io).unwrap().cutoff.unwrap();
    assert_eq!(found.offset, frame);
}

#[test]
fn a_bad_metadata_sector_in_a_frame_keeps_the_frame_and_its_header() {
    let (file, mut io) = wal(4);
    let mut intact = [0u8; 24];
    file.read(&mut io, 32 + 4120, &mut intact).unwrap();
    let frame = file.profile.slot_offset(1).unwrap();
    io.bad_sector = Some((frame, crate::error::IOERR_CORRUPTFS));
    let scanned = scan(&file, &mut io).unwrap();
    assert_eq!(scanned.cutoff, None);
    assert_eq!(scanned.observations.len(), 1);
    let mut survived = [0u8; 24];
    file.read(&mut io, 32 + 4120, &mut survived).unwrap();
    assert_eq!(survived, intact);
}

#[cfg(feature = "copy")]
#[test]
fn a_bad_sector_on_either_copy_keeps_the_frame() {
    for at in [0, 4096] {
        let (file, mut io) = wal(4);
        let frame = file.profile.slot_offset(1).unwrap();
        io.bad_sector = Some((frame + at, crate::error::IOERR_CORRUPTFS));
        let scanned = scan(&file, &mut io).unwrap();
        assert_eq!(scanned.cutoff, None);
        assert_eq!(scanned.observations.len(), 1);
    }
}

/// A new generation whose pages differ from `wal`'s, so an old sector cannot pass for new.
fn next_generation(file: &mut Mapped, io: &mut Io, frames: u64) {
    let next = [9u8; 8];
    file.write(io, 0, &header_with(4096, false, next)).unwrap();
    for frame in 0..frames {
        let mut header = [0u8; 24];
        header[..4].copy_from_slice(&1u32.to_be_bytes());
        header[4..8].copy_from_slice(&1u32.to_be_bytes());
        header[8..16].copy_from_slice(&next);
        file.write(io, 32 + frame * 4120, &header).unwrap();
        file.write(io, 32 + frame * 4120 + 24, &filled(0x33, 4096))
            .unwrap();
    }
}

fn old_bytes(file: &Mapped, io: &Io, block: u64, skip: u64, count: usize) -> Vec<u8> {
    let mut old = vec![0; count];
    let start = file.profile.slot_offset(block).unwrap() + skip;
    io.visible.bytes(start, &mut old);
    old
}

#[test]
fn a_page_sector_cannot_vouch_for_a_frame_of_another_generation() {
    let (mut file, mut io) = wal(3);
    let old = old_bytes(&file, &io, 1, 4096, 4096);
    next_generation(&mut file, &mut io, 3);
    let start = file.profile.slot_offset(1).unwrap();
    io.visible.write(start + 4096, &old);
    file.record_backfill(&mut io, 3).unwrap();
    let error = scan(&file, &mut io).err().unwrap();
    assert_eq!(error.code, CORRUPT_BACKFILLED);
    file.record_backfill(&mut io, 1).unwrap();
    assert_eq!(cutoff(&file, &mut io).offset, start);
}

#[cfg(feature = "copy")]
#[test]
fn a_lone_copy_cannot_vouch_for_a_frame_of_another_generation() {
    for (lost, kept) in [(0, 4096), (4096, 0)] {
        let (mut file, mut io) = wal(3);
        let old = old_bytes(&file, &io, 1, 0, 8192);
        next_generation(&mut file, &mut io, 3);
        let start = file.profile.slot_offset(1).unwrap();
        // A write of this slot left one old copy, and damage took the new one.
        io.visible
            .write(start + kept, &old[kept as usize..kept as usize + 4096]);
        io.visible.write(start + lost, &[0x5a; 4096]);
        assert_eq!(cutoff(&file, &mut io).offset, start);
        file.record_backfill(&mut io, 3).unwrap();
        assert_eq!(scan(&file, &mut io).err().unwrap().code, CORRUPT_BACKFILLED);
    }
}

#[cfg(feature = "copy")]
#[test]
fn copies_that_differ_in_the_tail_are_cut_and_below_the_watermark_are_refused() {
    let (mut file, mut io) = wal(3);
    let old = old_bytes(&file, &io, 2, 0, 4096);
    next_generation(&mut file, &mut io, 3);
    let start = file.profile.slot_offset(2).unwrap();
    // One copy of the newest frame landed before the crash, and the other did not.
    io.visible.write(start, &old);
    assert_eq!(cutoff(&file, &mut io).offset, start);
    file.record_backfill(&mut io, 3).unwrap();
    assert_eq!(scan(&file, &mut io).err().unwrap().code, CORRUPT_BACKFILLED);
}

#[cfg(feature = "copy")]
#[test]
fn a_tail_with_only_one_copy_is_cut() {
    let (file, io) = wal(1);
    let offset = file.profile.slot_offset(1).unwrap();
    for tail in [4096, 4097, 8191] {
        let mut broken = io.clone();
        broken.truncate(offset + tail).unwrap();
        assert_eq!(cutoff(&file, &mut broken).offset, offset);
    }
}

#[test]
fn a_finished_commit_proves_a_stale_frame_before_it_was_lost() {
    let (mut file, mut io) = wal(5);
    let old = old_bytes(&file, &io, 1, 0, 8192);
    let next = [9u8; 8];
    file.write(&mut io, 0, &header_with(4096, false, next))
        .unwrap();
    for frame in 0..5 {
        append(&mut file, &mut io, frame, next, true);
    }
    let start = file.profile.slot_offset(1).unwrap();
    io.visible.write(start, &old);
    file.record_backfill(&mut io, 5).unwrap();
    let error = scan(&file, &mut io).err().unwrap();
    assert_eq!(error.code, CORRUPT_BACKFILLED);
    file.record_backfill(&mut io, 1).unwrap();
    let found = cutoff(&file, &mut io);
    assert_eq!(found.offset, start);
    assert_eq!((found.later_frames, found.lost_commits), (3, 2));
}

#[test]
fn a_wal_that_ends_below_the_watermark_is_refused() {
    let (file, mut io) = wal(5);
    file.record_backfill(&mut io, 4).unwrap();
    io.truncate(file.profile.slot_offset(2).unwrap()).unwrap();
    let error = scan(&file, &mut io).err().unwrap();
    assert_eq!(error.code, CORRUPT_BACKFILLED);
}

#[test]
fn a_wal_truncated_by_a_finished_checkpoint_is_not_refused() {
    let (file, mut io) = wal(5);
    file.record_backfill(&mut io, 5).unwrap();
    io.truncate(file.profile.content_start()).unwrap();
    assert_eq!(scan(&file, &mut io).unwrap().cutoff, None);
}

#[test]
fn a_dead_tail_under_an_overstated_watermark_is_not_refused() {
    let (mut file, mut io) = wal(5);
    next_generation(&mut file, &mut io, 2);
    // A private wal-index has only an upper bound, which can cover dead frames.
    file.record_backfill(&mut io, 5).unwrap();
    assert_eq!(scan(&file, &mut io).unwrap().cutoff, None);
}

#[test]
fn a_stale_frame_with_only_a_last_commit_after_it_is_an_ordinary_tail() {
    let (mut file, mut io) = wal(4);
    let next = [9u8; 8];
    file.write(&mut io, 0, &header_with(4096, false, next))
        .unwrap();
    for frame in [0, 1, 3] {
        append(&mut file, &mut io, frame, next, true);
    }
    file.record_backfill(&mut io, 2).unwrap();
    assert_eq!(scan(&file, &mut io).unwrap().cutoff, None);
}

/// A watermark over `old` frames, then a new header with `new` frames of its own.
fn restarted(old: u64, new: u64) -> (Mapped, Io) {
    let (mut file, mut io) = wal(old);
    file.record_backfill(&mut io, old as u32).unwrap();
    file.write(&mut io, 0, &header_with(4096, false, NEXT))
        .unwrap();
    for frame in 0..new {
        append(&mut file, &mut io, frame, NEXT, true);
    }
    (file, io)
}

fn watermarked() -> (Mapped, Io) {
    let (file, mut io) = wal(3);
    file.record_backfill(&mut io, 3).unwrap();
    (file, io)
}

#[test]
fn the_first_frame_lets_recovery_do_without_an_unreadable_record() {
    for (mut image, sector, commits) in [
        // What a power cut leaves: the record being written, which nothing depends on yet.
        (restarted(3, 0), CURRENT, 3),
        (watermarked(), PREVIOUS, 3),
        // Rot in the older record.
        (restarted(3, 2), PREVIOUS, 2),
    ] {
        let (file, io) = (&image.0, &mut image.1);
        io.bad_sector = Some((sector, IOERR_CORRUPTFS));
        assert!(scan(file, io).ok().unwrap().cutoff.is_none());
        assert_eq!(last_commit(file, io).unwrap(), commits);
    }
}

#[test]
fn an_unreadable_record_that_may_be_current_stops_recovery() {
    // Falling back would hide the frames of a shorter generation, and of a longer one with
    // nothing else to give it away, as a read error would not.
    for (old, new) in [(5, 2), (3, 4)] {
        let (file, mut io) = restarted(old, new);
        io.bad_sector = Some((CURRENT, IOERR_CORRUPTFS));
        assert_eq!(scan(&file, &mut io).err().unwrap().code, IOERR_CORRUPTFS);
    }
    let (file, mut io) = wal(2);
    io.bad_sector = Some((CURRENT, IOERR_CORRUPTFS));
    assert_eq!(scan(&file, &mut io).err().unwrap().code, IOERR_CORRUPTFS);
}

#[test]
fn a_first_frame_that_cannot_vouch_for_the_other_record_keeps_the_read_error() {
    for damaged in 0..3 {
        let (file, mut io) = watermarked();
        let slot = file.profile.slot_offset(0).unwrap();
        match damaged {
            0 => damage(&file, &mut io, 0, 20),
            1 => io.visible.write(slot, &[0; 8192]),
            _ => {
                io.visible.write(slot, &[0xa5; 4096]);
                let physical = io.size().unwrap();
                assert!(file.slot(&mut io, 0, physical).unwrap().0.rebuilt);
            }
        }
        io.bad_sector = Some((PREVIOUS, IOERR_CORRUPTFS));
        assert_eq!(scan(&file, &mut io).err().unwrap().code, IOERR_CORRUPTFS);
    }
}

#[test]
fn with_no_frames_an_unreadable_record_hides_nothing() {
    let (mut file, mut io) = wal(0);
    file.write(&mut io, 0, &header(4096, false)).unwrap();
    io.bad_sector = Some((CURRENT, IOERR_CORRUPTFS));
    assert!(file.control(&mut io).unwrap().is_none());
    assert!(scan(&file, &mut io).ok().unwrap().cutoff.is_none());
}

#[test]
fn a_checkpoint_cannot_write_an_older_header_over_the_one_it_lost() {
    let (file, mut io) = restarted(3, 4);
    io.bad_sector = Some((CURRENT, IOERR_CORRUPTFS));
    io.events.clear();
    let error = file.record_backfill(&mut io, 4).unwrap_err();
    assert_eq!(error.code, IOERR_CORRUPTFS);
    assert_eq!(io.write_count(), 0);
}

#[test]
fn the_next_control_write_lands_on_the_unreadable_record() {
    let (file, mut io) = watermarked();
    io.bad_sector = Some((PREVIOUS, IOERR_CORRUPTFS));
    file.record_backfill(&mut io, 3).unwrap();
    assert!(
        io.events
            .iter()
            .any(|event| matches!(event, Event::Write(PREVIOUS, _)))
    );
    io.bad_sector = None;
    let control = file.control(&mut io).unwrap().unwrap();
    assert_eq!((control.sequence, control.backfill_frames), (2, 3));
}

#[test]
fn last_commit_is_where_sqlite_recovery_ends() {
    let (file, mut io) = wal(5);
    assert_eq!(last_commit(&file, &mut io).unwrap(), 5);
}

#[test]
fn frames_with_no_commit_after_them_do_not_count() {
    let (mut file, mut io) = wal(3);
    append(&mut file, &mut io, 3, SALTS, false);
    append(&mut file, &mut io, 4, SALTS, false);
    assert_eq!(last_commit(&file, &mut io).unwrap(), 3);
}

#[test]
fn dead_frames_of_a_reused_wal_do_not_count() {
    let (mut file, mut io) = wal(6);
    let next = [9u8; 8];
    file.write(&mut io, 0, &header_with(4096, false, next))
        .unwrap();
    append(&mut file, &mut io, 0, next, true);
    append(&mut file, &mut io, 1, next, true);
    assert_eq!(last_commit(&file, &mut io).unwrap(), 2);
}

#[test]
fn a_frame_that_breaks_the_chain_ends_the_count() {
    let (mut file, mut io) = wal(3);
    append(&mut file, &mut io, 3, SALTS, false);
    append(&mut file, &mut io, 4, SALTS, true);
    let mut frame = vec![0; 4120];
    file.read(&mut io, 32 + 3 * 4120, &mut frame).unwrap();
    frame[16] ^= 1;
    file.write(&mut io, 32 + 3 * 4120, &frame).unwrap();
    assert_eq!(last_commit(&file, &mut io).unwrap(), 3);
}

#[test]
fn a_write_counts_only_a_commit_it_finished() {
    let (mut file, mut io) = wal(3);
    assert_eq!(file.last_commit, 3);
    append(&mut file, &mut io, 3, SALTS, false);
    assert_eq!(file.last_commit, 3);
    append(&mut file, &mut io, 4, SALTS, true);
    assert_eq!(file.last_commit, 5);
    file.write(&mut io, 0, &header_with(4096, false, [9u8; 8]))
        .unwrap();
    assert_eq!(file.last_commit, 0);
}

#[cfg(feature = "copy")]
#[test]
fn a_surviving_newest_control_half_keeps_its_checkpoint_watermark() {
    for stale_half in 0..2 {
        let (file, mut io) = wal(3);
        file.record_backfill(&mut io, 1).unwrap();
        let mut older = vec![0; HEADER_RECORD_SIZE];
        io.visible.bytes(PREVIOUS, &mut older);
        file.record_backfill(&mut io, 2).unwrap();
        file.record_backfill(&mut io, 3).unwrap();
        let width = HEADER_RECORD_SIZE / 2;
        io.visible
            .write(PREVIOUS + (stale_half * width) as u64, &older[..width]);
        let control = file.control(&mut io).unwrap().unwrap();
        assert_eq!((control.sequence, control.backfill_frames), (4, 3));
        damage(&file, &mut io, 2, 20);
        io.events.clear();
        assert_eq!(scan(&file, &mut io).err().unwrap().code, CORRUPT_BACKFILLED);
        assert_eq!(io.write_count(), 0);
    }
}

#[test]
fn foreign_control_refusal_precedes_cutoff_or_last_commit_selection() {
    for frame_damage in [false, true] {
        let (file, mut io) = wal(3);
        let mut foreign = file.control(&mut io).unwrap().unwrap();
        foreign.sequence += 1;
        foreign.wal_header = header_with(4096, false, NEXT);
        let record = crate::codec::encode_control(&[0x55; 16], &foreign).unwrap();
        io.visible.write(PREVIOUS, &record);
        if frame_damage {
            damage(&file, &mut io, 1, 20);
        }
        let before = io.visible.clone();
        io.events.clear();
        assert_eq!(
            scan(&file, &mut io).err().unwrap().code,
            crate::error::CORRUPT
        );
        assert_eq!(
            last_commit(&file, &mut io).unwrap_err().code,
            crate::error::CORRUPT
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

#[test]
fn empty_wals_need_no_control_but_refuse_a_proven_foreign_control() {
    for had_frames in [false, true] {
        let (file, mut io) = wal(if had_frames { 3 } else { 0 });
        io.truncate(file.profile.content_start()).unwrap();
        assert_eq!(scan(&file, &mut io).unwrap().cutoff, None);
        let control = crate::codec::Control {
            sequence: 2,
            wal_header: header(4096, false),
            ..crate::codec::Control::default()
        };
        let local = crate::codec::encode_control(&file.profile.id, &control).unwrap();
        io.visible.write(PREVIOUS, &local);
        assert_eq!(scan(&file, &mut io).unwrap().cutoff, None);
        let foreign = crate::codec::encode_control(&[0x55; 16], &control).unwrap();
        io.visible.write(PREVIOUS, &foreign);
        io.events.clear();
        assert_eq!(
            scan(&file, &mut io).err().unwrap().code,
            crate::error::CORRUPT
        );
        assert!(
            io.events
                .iter()
                .all(|event| matches!(event, Event::Read(..) | Event::Size))
        );
    }
}
