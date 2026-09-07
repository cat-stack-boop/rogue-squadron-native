"""Malformed-input checks for the cartridge and archive inspection boundary."""
import struct
import tempfile
import unittest
from pathlib import Path
from extract_assets import parse
from inspect_rom import inspect

ROOT = Path(__file__).resolve().parents[1]


class SafetyChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rom = (ROOT/'roms/rogue_squadron.us.rev1.z64').read_bytes()
        cls.start, cls.blocks, cls.entries = parse(cls.rom)

    def test_traversal_name_rejected(self):
        bad = bytearray(self.rom)
        p = self.blocks[0]['manifest_offset'] + 16
        bad[p:p+16] = b'../../escape\0\0\0\0'
        with self.assertRaisesRegex(ValueError, 'Unsafe asset name'):
            parse(bad)

    def test_manifest_outside_cartridge_rejected(self):
        bad = bytearray(self.rom)
        struct.pack_into('>I', bad, self.blocks[0]['offset'], 0xffffffff)
        with self.assertRaisesRegex(ValueError, 'Manifest out of bounds'):
            parse(bad)

    def test_empty_directory_extent_rejected(self):
        bad = bytearray(self.rom)
        p = self.blocks[0]['manifest_offset']
        bad[p+12] = 0x80
        struct.pack_into('>H', bad, p+14, 0)
        with self.assertRaisesRegex(ValueError, 'Directory extends'):
            parse(bad)

    def test_corrupted_game_code_rejected(self):
        bad = bytearray(self.rom)
        bad[0x2000] ^= 1
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'bad.z64'
            p.write_bytes(bad)
            with self.assertRaisesRegex(ValueError, 'N64 checksum'):
                inspect(p)

    def test_host_executable_header_rejected(self):
        bad = bytearray(self.rom)
        bad[:4] = bytes.fromhex('cffaedfe')
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'fake.z64'
            p.write_bytes(bad)
            with self.assertRaisesRegex(ValueError, 'not a host executable'):
                inspect(p)


if __name__ == '__main__':
    unittest.main()
