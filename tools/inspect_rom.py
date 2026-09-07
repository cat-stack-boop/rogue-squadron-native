"""Identify a cartridge and verify its N64 header checksum without executing it."""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

MASK = 0xFFFFFFFF


def checksum(data, cic):
    seed = {6102: 0xF8CA4DDC, 6103: 0xA3886759, 6105: 0xDF26F436, 6106: 0x1FEA617A}[cic]
    t1 = t2 = t3 = t4 = t5 = t6 = seed
    for i in range(0x1000, 0x101000, 4):
        d = struct.unpack_from('>I', data, i)[0]
        if t6 + d > MASK:
            t4 = (t4 + 1) & MASK
        t6 = (t6 + d) & MASK
        t3 ^= d
        s = d & 31
        r = ((d << s) | (d >> ((32 - s) & 31))) & MASK
        t5 = (t5 + r) & MASK
        t2 ^= r if t2 > d else t6 ^ d
        v = struct.unpack_from('>I', data, 0x750 + (i & 255))[0] if cic == 6105 else t5
        t1 = (t1 + (v ^ d)) & MASK
    if cic == 6103:
        return ((t6 ^ t4) + t3) & MASK, ((t5 ^ t2) + t1) & MASK
    if cic == 6106:
        return (t6 * t4 + t3) & MASK, (t5 * t2 + t1) & MASK
    return t6 ^ t4 ^ t3, t5 ^ t2 ^ t1


def inspect(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('Expected a regular ROM file, not a symlink')
    if path.stat().st_size != 16 * 1024 * 1024:
        raise ValueError('Unexpected size for this Rogue Squadron cartridge')
    data = path.read_bytes()
    if data[:4] != bytes.fromhex('80371240'):
        raise ValueError('Expected a big-endian N64 cartridge, not a host executable')
    expected = struct.unpack_from('>II', data, 0x10)
    matches = [cic for cic in (6102, 6103, 6105, 6106) if checksum(data, cic) == expected]
    if len(matches) != 1:
        raise ValueError(f'N64 checksum did not identify one valid CIC: {matches}')
    return {
        'file': path.name, 'bytes': len(data),
        'sha1': hashlib.sha1(data).hexdigest(),
        'sha256': hashlib.sha256(data).hexdigest(),
        'title': data[0x20:0x34].decode('ascii').strip(),
        'cartridge_id': data[0x3B:0x3F].decode('ascii'),
        'revision': data[0x3F], 'entrypoint': hex(struct.unpack_from('>I', data, 8)[0]),
        'header_crc': [f'{x:08x}' for x in expected], 'cic': matches[0],
        'boot_crc32': f'{zlib.crc32(data[0x40:0x1000]):08x}',
        'checksum_valid': True,
        'assessment': 'Valid cartridge structure and N64 checksum; not a malware certification. Read as data only.',
    }


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('rom', type=Path)
    p.add_argument('--report', type=Path)
    args = p.parse_args()
    result = json.dumps(inspect(args.rom), indent=2) + '\n'
    if args.report:
        args.report.write_text(result)
    print(result, end='')
