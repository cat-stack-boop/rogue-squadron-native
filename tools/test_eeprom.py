"""Exercise host save recovery with a verified fixture and disposable files."""
from pathlib import Path
import hashlib,json,struct,subprocess,tempfile,zlib

ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    original=Path.home()/'Library/Application Support/Rogue Squadron Native/saves/rogue-squadron.eep'
    before=sha(original);data=original.read_bytes()
    assert len(data)==512 and all(struct.unpack_from('>I',data,o)[0]==zlib.adler32(data[s:s+n])
        for o,s,n in [(0,4,12),(16,20,12),(0x24,0x28,16),(0x20,0x38,176),(0xec,0xf0,16),(0xe8,0x100,176)])
    executable=ROOT/'build/game/rogue_eeprom_probe'
    report={'passed':False,'original_save_sha256':before}
    try:
        parent=ROOT/'runtime/app-tests';parent.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='eeprom-',dir=parent) as folder:
            fixture=Path(folder)/'fixture.eep';fixture.write_bytes(data)
            result=subprocess.run([str(executable),str(fixture),str(Path(folder)/'cases')],
                capture_output=True,text=True,timeout=20,cwd=ROOT)
            (ROOT/'reports/eeprom-probe.log').write_text(result.stdout+result.stderr)
            if result.returncode:raise RuntimeError(result.stdout+result.stderr)
            report.update(json.loads(result.stdout.splitlines()[-1]))
            report['passed']=False
            report['probe_sha256']=sha(executable)
            codec=ROOT/'build/game/rogue_save_codec_probe'
            result=subprocess.run([str(codec),str(fixture),str(Path(folder)/'codec')],capture_output=True,text=True,timeout=20,cwd=ROOT)
            (ROOT/'reports/save-codec-probe.log').write_text(result.stdout+result.stderr)
            if result.returncode:raise RuntimeError(result.stdout+result.stderr)
            report['original_codec']=json.loads(result.stdout.splitlines()[-1])
            report['codec_probe_sha256']=sha(codec)
            assert report['original_codec']['passed']
            report['game_library_sha256']=sha(ROOT/'build/game/librogue_game.a')
            report['source_sha256']={name:sha(ROOT/name) for name in ['src/native_eeprom.cpp','src/native_eeprom.hpp','src/eeprom_probe.cpp','src/save_codec_probe.cpp']}
        report['original_save_unchanged']=sha(original)==before
        assert report['original_save_unchanged']
        report['passed']=True
    finally:
        (ROOT/'reports/eeprom-store-verification.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2))

if __name__=='__main__':main()
