"""Independently verify the original game's native-written EEPROM checksums."""
import argparse,hashlib,json,struct,zlib
from pathlib import Path

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('save',nargs='?',type=Path,default=root/'runtime/saves/rogue-squadron.eep')
a=p.parse_args();b=a.save.read_bytes()
if len(b)!=512:raise ValueError('Expected the cartridge gamesave asset\'s 512-byte size')
checks=[]
for label,offset,start,size in [('header_system',0,4,12),('header_game',16,20,12),
    ('copy1_metadata',0x24,0x28,16),('copy1_body',0x20,0x38,176),
    ('copy2_metadata',0xec,0xf0,16),('copy2_body',0xe8,0x100,176)]:
    stored=struct.unpack_from('>I',b,offset)[0];actual=zlib.adler32(b[start:start+size],1)
    checks.append({'label':label,'offset':hex(offset),'stored':f'{stored:08x}',
                   'calculated':f'{actual:08x}','matches':stored==actual})
report={'file':str(a.save),'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest(),
        'checks':checks,'all_checksums_valid':all(c['matches'] for c in checks),
        'campaign_save_restore_verified':False}
(root/'reports/eeprom-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
raise SystemExit(0 if report['all_checksums_valid'] else 1)
