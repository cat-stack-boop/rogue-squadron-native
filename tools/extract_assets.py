"""Bounds-checked asset extraction; discovers the archive instead of using Rev 0 offsets.

Format reference: Tmcg2/rogue_squadron64 docs/data_blob/data_blob.md.
Every compressed stream must pass zlib checksum, EOF, and output-size checks.
"""
import argparse
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path
from inspect_rom import inspect

NAME = re.compile(r'[A-Za-z0-9_. -]{1,16}\Z')
MAX_ASSET = 32 * 1024 * 1024
MAX_TOTAL = 256 * 1024 * 1024


def name(raw):
    value = raw.split(b'\0', 1)[0].decode('ascii')
    if not NAME.fullmatch(value) or value in ('.', '..'):
        raise ValueError(f'Unsafe asset name: {value!r}')
    return value


def parse(data):
    candidates = [m.start() for m in re.finditer(b'data' + b'\0' * 12, data)
                  if data[m.start() + 32:m.start() + 48] == b'dbg_data' + b'\0' * 8]
    if len(candidates) != 1:
        raise ValueError(f'Archive header is ambiguous: {candidates}')
    start = candidates[0]
    entries = []
    blocks = []
    for i in range(2):
        header = start + i * 32
        segment = name(data[header:header + 16])
        base = start + 64 + struct.unpack_from('>I', data, header + 28)[0]
        if not 0 <= base <= len(data) - 8:
            raise ValueError('Block header out of bounds')
        size, flags, manifest_size = struct.unpack_from('>IHH', data, base)
        manifest = base + size
        if size < 8 or manifest_size % 32 or manifest + manifest_size > len(data):
            raise ValueError('Manifest out of bounds or unaligned')
        blocks.append({'name': segment, 'offset': base, 'manifest_offset': manifest,
                       'manifest_size': manifest_size, 'flags': flags})
        def directory(first, end, parents):
            index = first
            while index < end:
                pos = manifest + index * 32
                off, unpacked, packed, flag, unk, directory_size, raw_name = struct.unpack_from('>IIIBBH16s', data, pos)
                n = name(raw_name)
                if flag & 0x80:
                    count = directory_size // 32
                    if directory_size % 32 or count < 1 or index + count > end:
                        raise ValueError('Directory extends beyond parent')
                    directory(index + 1, index + count, parents + [n])
                    index += count
                else:
                    compressed = packed != 0xFFFFFFFF
                    stored = packed - 10 if compressed else unpacked
                    begin = base + off
                    if stored < 0 or unpacked > MAX_ASSET or begin < base + 8 or begin + stored > manifest:
                        raise ValueError(f'Asset extent out of bounds: {parents + [n]}')
                    entries.append({'path': '/'.join(parents + [n]), 'rom_offset': begin,
                                    'stored_bytes': stored, 'bytes': unpacked,
                                    'compressed': compressed, 'flags': flag})
                    index += 1
        directory(0, manifest_size // 32, [segment])
    paths = [e['path'] for e in entries]
    if len(set(paths)) != len(paths):
        raise ValueError('Duplicate asset paths')
    if sum(e['bytes'] for e in entries) > MAX_TOTAL:
        raise ValueError('Archive exceeds extraction budget')
    return start, blocks, entries


def extract(rom, output, report):
    identity = inspect(rom)
    data = rom.read_bytes()
    start, blocks, entries = parse(data)
    root = output.resolve()
    total = 0
    for entry in entries:
        off = entry['rom_offset']
        raw = data[off:off + entry['stored_bytes']]
        if entry['compressed']:
            stream = zlib.decompressobj()
            decoded = stream.decompress(raw, entry['bytes'] + 1)
            if not stream.eof or stream.unconsumed_tail or stream.unused_data:
                raise ValueError(f'Incomplete or trailing compressed data: {entry["path"]}')
        else:
            decoded = raw
        if len(decoded) != entry['bytes']:
            raise ValueError(f'Decoded size mismatch: {entry["path"]}')
        target = root / entry['path']
        if not target.resolve().is_relative_to(root):
            raise ValueError('Asset escapes destination')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(decoded)
        entry['sha256'] = hashlib.sha256(decoded).hexdigest()
        total += len(decoded)
    result = {'rom_sha256': identity['sha256'], 'archive_offset': start,
              'blocks': blocks, 'asset_count': len(entries), 'total_bytes': total, 'assets': entries}
    report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'assets'}, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('rom', type=Path)
    p.add_argument('--output', type=Path, default=Path('assets'))
    p.add_argument('--report', type=Path, default=Path('reports/assets.json'))
    a = p.parse_args()
    extract(a.rom, a.output, a.report)
