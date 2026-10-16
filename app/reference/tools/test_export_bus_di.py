"""用确定变位向量和坏帧检查VCD导出，不连接装置。"""
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
from export_bus_di import export


def frame(sequence, ticks, raw):
    data = bytearray(256)
    struct.pack_into('<QIIIQ', data, 4, sequence, 7, 1, 1 << 7, ticks)
    struct.pack_into('<IHHI', data, 48, 600, 0, 1, 0)
    struct.pack_into('<H', data, 224, raw)
    struct.pack_into('<H', data, 232, 0xffff)
    struct.pack_into('<I', data, 240, zlib.crc32(data[64:240]))
    return bytes(data)


class ExportTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def check_capture(self, body):
        source = self.root / 'capture.bin'
        source.write_bytes(body)
        return export(source, self.root / 'capture.vcd', self.root / 'capture.csv')

    def test_all_bits_and_actual_time(self):
        report = self.check_capture(frame(1, 100, 0) + frame(2, 703, 0x8001) + frame(3, 1301, 0x8000))
        self.assertEqual(report['frames'], 3)
        self.assertEqual((report['min_interval_ticks'], report['max_interval_ticks']), (598, 603))
        vcd = (self.root / 'capture.vcd').read_text()
        self.assertIn('#25125\n1d0\n1d15\n', vcd)
        self.assertIn('#50042\n0d0\n', vcd)
        self.assertIn('703,25125,0x8001,0xffff', (self.root / 'capture.csv').read_text())

    def test_reject_crc(self):
        bad = bytearray(frame(1, 100, 1)); bad[224] ^= 1
        with self.assertRaisesRegex(ValueError, 'CRC'):
            self.check_capture(bad)

    def test_reject_truncation(self):
        with self.assertRaisesRegex(ValueError, '不完整'):
            self.check_capture(frame(1, 100, 1)[:-1])

    def test_reject_missing_sequence(self):
        with self.assertRaisesRegex(ValueError, '不连续'):
            self.check_capture(frame(1, 100, 1) + frame(3, 1300, 2))

    def test_reject_session_change(self):
        bad = bytearray(frame(2, 700, 2)); struct.pack_into('<I', bad, 12, 9)
        with self.assertRaisesRegex(ValueError, '不连续'):
            self.check_capture(frame(1, 100, 1) + bad)

    def test_reject_adc_or_nonfixed(self):
        for offset, value in ((20, (1 << 7) | 1), (48, 0)):
            with self.subTest(offset=offset), tempfile.TemporaryDirectory() as temp:
                self.root = Path(temp)
                bad = bytearray(frame(1, 100, 1)); struct.pack_into('<I', bad, offset, value)
                with self.assertRaises(ValueError):
                    self.check_capture(bad)

    def test_never_overwrite(self):
        self.check_capture(frame(1, 100, 1))
        with self.assertRaises(FileExistsError):
            self.check_capture(frame(1, 100, 2))


if __name__ == '__main__':
    unittest.main()
