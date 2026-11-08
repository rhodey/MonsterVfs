"""Mapping tests keep logical fault identities separate from physical layout."""
from pathlib import Path
import runpy
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
MATRIX = runpy.run_path(str(ROOT / 'tools/test-recovery-matrix'))


class RecoveryMatrixTests(unittest.TestCase):
    def test_main_payload_copy_locations(self):
        value = b'payload' * 100
        for codec, start, offsets in [('native', 0, [0]), ('parity', 8192, [4096]),
                                      ('copy', 8192, [0, 4096])]:
            raw = bytearray(start + (4096 if codec == 'native' else 8192))
            for offset in offsets:
                raw[start + offset + 100:start + offset + 100 + len(value)] = value
            target = MATRIX['locate'](raw, codec, False, value)
            self.assertEqual(target['page'], 1)
            self.assertEqual(target['positions'], [start + offset + 140 for offset in offsets])

    def test_wal_frame_headers_and_commit_boundaries(self):
        value = b'row' * 100
        for codec in ('native', 'parity', 'copy'):
            start, width = (32, 4120) if codec == 'native' else (16384, 8192)
            raw = bytearray(start + width * 2)
            for index in range(2):
                begin = start + index * width
                header = begin if codec == 'native' else begin + (4072 if codec == 'parity' else 4068)
                struct.pack_into('>II', raw, header, 7 + index, 100)
                pages = [begin + 24] if codec == 'native' else [begin + 4096] if codec == 'parity' else [begin, begin + 4096]
                for page in pages:
                    raw[page + 200:page + 200 + len(value)] = value
            target = MATRIX['locate'](raw, codec, True, value, 2)
            self.assertEqual((target['index'], target['page'], target['transaction']), (2, 8, 2))

    def test_differing_pristine_copies_are_rejected(self):
        raw = bytearray(16384)
        raw[8192 + 100:8192 + 103] = b'row'
        with self.assertRaisesRegex(ValueError, 'copies differ'):
            MATRIX['locate'](raw, 'copy', False, b'row')

    def test_control_selection_records_lost_or_mirrored_evidence(self):
        raw = bytearray(16384)
        for offset, sequence, watermark in ((8192, 4, 0), (12288, 5, 80)):
            struct.pack_into('<Q', raw, offset + 8, sequence)
            struct.pack_into('<I', raw, offset + 56, watermark)
            raw[offset + 2048:offset + 2176] = raw[offset:offset + 128]
        newest, older = MATRIX['newest_control'](raw, 'copy')
        self.assertEqual(newest['offset'], 12288)
        self.assertEqual((older['watermark'], newest['watermark']), (0, 80))
        struct.pack_into('<I', raw, 8192 + 56, 80)
        raw[8192 + 2048:8192 + 2176] = raw[8192:8192 + 128]
        newest, older = MATRIX['newest_control'](raw, 'copy')
        self.assertEqual((older['watermark'], newest['watermark']), (80, 80))

    def test_old_wal_epoch_watermark_is_not_current_evidence(self):
        raw = bytearray(16384)
        for offset, sequence, watermark in ((8192, 4, 80), (12288, 3, 156)):
            struct.pack_into('<Q', raw, offset + 8, sequence)
            struct.pack_into('<I', raw, offset + 56, watermark)
            raw[offset + 32:offset + 40] = b'newepoch'
            raw[offset + 48:offset + 56] = b'newepoch' if sequence == 4 else b'oldepoch'
        newest, older = MATRIX['newest_control'](raw, 'parity')
        self.assertEqual(newest['watermark'], 80)
        self.assertEqual((older['recorded_watermark'], older['watermark']), (156, 0))


if __name__ == '__main__':
    unittest.main()
