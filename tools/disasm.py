"""Inspect main-segment instructions in the verified Revision 1 cartridge."""
import argparse,json
from pathlib import Path
from capstone import Cs,CS_ARCH_MIPS,CS_MODE_MIPS64,CS_MODE_BIG_ENDIAN
p=argparse.ArgumentParser();p.add_argument('address',type=lambda s:int(s,16));p.add_argument('size',type=lambda s:int(s,16),nargs='?',default=0x100)
p.add_argument('--section',default='main',choices=['main','mission','menu','cinematic'])
a=p.parse_args();b=Path('roms/rogue_squadron.us.rev1.z64').read_bytes()
section=next(s for s in json.loads(Path('config/segments.json').read_text())['sections'] if s['name']==a.section)
base=int(section['vram'],16);length=int(section['size'],16)
if not base<=a.address<base+length:raise ValueError('Address is outside the selected code section')
o=int(section['rom'],16)+a.address-base
c=Cs(CS_ARCH_MIPS,CS_MODE_MIPS64|CS_MODE_BIG_ENDIAN);c.skipdata=True
for i in c.disasm(b[o:o+a.size],a.address):print(f'{i.address:08x} {i.bytes.hex()} {i.mnemonic:10} {i.op_str}')
