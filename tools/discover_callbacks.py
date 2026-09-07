"""Find address-taken main-code entries with explicit references and boundaries.

Only LUI + ADDIU/ORI constructions along a short straight-line sequence qualify.
Entries must follow a return and start with a frame prologue or a short leaf
function. This supplements direct-call discovery; it does not infer behaviour.
"""
import bisect,hashlib,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
b=(ROOT/'roms/rogue_squadron.us.rev1.z64').read_bytes()
layout=json.loads((ROOT/'config/segments.json').read_text())
if hashlib.sha256(b).hexdigest()!=layout['rom_sha256']:raise ValueError('ROM mismatch')
sections=json.loads((ROOT/'reports/code-analysis.json').read_text())['sections']
analysis=sections[0]['candidates']
valid=[f for section in sections for f in section['candidates'] if not f['unknown_instructions'] and not f['known_data']]
valid.sort(key=lambda f:f['rom']);starts=[f['rom'] for f in valid]
known={f['vram'] for f in analysis if not f['unknown_instructions'] and not f['known_data']}
def word(o):return struct.unpack_from('>I',b,o)[0]
def source_valid(o):
    i=bisect.bisect_right(starts,o)-1
    return i>=0 and o<valid[i]['rom']+valid[i]['size']
def entry_evidence(address):
    p=address-0x80000000+0xc00
    previous=next((p-d for d in (8,12,16,20) if word(p-d)==0x03e00008),None)
    if previous is None:return None
    # Padding after the previous return must actually be NOPs.
    if any(word(q) for q in range(previous+8,p,4)):return None
    for delta in range(0,20,4):
        w=word(p+delta);op=w>>26
        if w&0xffff8000==0x27bd8000:return {'kind':'stack_frame','prologue':hex(address+delta),'previous_return':hex(previous-0xc00+0x80000000)}
        if w==0x03e00008:return {'kind':'leaf_return','size':hex(delta+8),'previous_return':hex(previous-0xc00+0x80000000)}
        if op in (1,2,3,4,5,6,7,20,21,22,23) or (op==0 and w&63 in (8,9)):return None
    return None
found={}
for off in range(0x1ed0,int(layout['archive'],16),4):
    initial=word(off)
    if initial>>26!=15 or not source_valid(off):continue
    register=(initial>>16)&31;high=(initial&65535)<<16;delay=False
    for distance in range(4,28,4):
        pos=off+distance;w=word(pos);op=w>>26;rs=(w>>21)&31;rt=(w>>16)&31;imm=w&65535
        if op in (9,13) and rs==register:
            address=((high+(imm if imm<32768 else imm-65536)) if op==9 else high|imm)&0xffffffff
            if 0x800012d0<=address<0x80091ea0 and address%4==0:
                evidence=entry_evidence(address)
                if evidence:
                    item=found.setdefault(address,{'vram':hex(address),**evidence,'references':[]})
                    item['references'].append({'lui_rom':hex(off),'low_rom':hex(pos)})
            break
        if delay:break
        if op in (1,2,3,4,5,6,7,20,21,22,23) or (op==0 and w&63 in (8,9)):
            if register==31:break
            delay=True;continue
        if w==0 or op in (40,41,43,57,61,63):continue # NOP or stores.
        if op in (9,13,15,32,33,35,36,37) and rt!=register:continue
        break
path=ROOT/'config/discovered-functions.json'
existing=json.loads(path.read_text()) if path.exists() else {'main':[]}
entries={int(e['vram'],16):e for e in existing['main']};added=[]
for address,item in found.items():
    if address not in known and address not in entries:
        entries[address]={k:v for k,v in item.items() if k!='kind'}
        entries[address]['evidence']='Address construction plus preceding return and '+item['kind']
        added.append(hex(address))
existing['main']=[entries[a] for a in sorted(entries)]
path.write_text(json.dumps(existing,indent=2)+'\n')
(ROOT/'reports/callback-discovery.json').write_text(json.dumps({'rom_sha256':layout['rom_sha256'],'new_entries':added,'total_retained':len(entries),'references':list(found.values())},indent=2)+'\n')
print(json.dumps({'added':len(added),'total':len(entries),'addresses':added},indent=2))
