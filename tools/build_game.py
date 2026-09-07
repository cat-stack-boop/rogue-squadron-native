"""Build current generated ARM64 code and record evidence for the exact sources."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

if __name__=='__main__':
    generation=json.loads((ROOT/'reports/candidate-generation.json').read_text())
    if not generation['code_generation_succeeded']:
        raise RuntimeError('Fix candidate generation before building the native game')
    for field,path in [('config_sha256','build/candidate.toml'),('symbols_sha256','build/candidate-symbols.toml')]:
        if generation[field]!=sha(ROOT/path):raise RuntimeError(f'Stale generation input: {path}')
    subprocess.run(['.venv/bin/python','tools/generate_dispatch.py'],cwd=ROOT,check=True)
    with (ROOT/'reports/native-game-build.log').open('w') as log:
        subprocess.run(['.venv/bin/cmake','-S','.','-B','build/game','-G','Ninja',
            '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OSX_ARCHITECTURES=arm64',
            f'-DCMAKE_MAKE_PROGRAM={ROOT}/.venv/bin/ninja'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
        result=subprocess.run(['.venv/bin/cmake','--build','build/game','-j','4'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
    report={'build_succeeded':result.returncode==0,'playability_assessed':False,'artifacts':{}}
    if result.returncode==0:
        for file in ['build/game/rogue_startup','build/game/rogue_result_fixture','build/game/librogue_game.a',
                     'build/game/vendor/N64ModernRuntime/ultramodern/libultramodern.a',
                     'build/gliden64/plugin/Release/mupen64plus-video-GLideN64.dylib']:
            p=ROOT/file
            report['artifacts'][file]={'sha256':sha(p),'bytes':p.stat().st_size,
                'architecture':subprocess.check_output(['lipo','-info',str(p)],text=True).strip()}
    sources=[ROOT/'CMakeLists.txt',ROOT/'tools/analyze_code.py',ROOT/'tools/generate_candidates.py']
    sources += [p for folder in ['src','config','patches'] for p in (ROOT/folder).iterdir() if p.is_file()]
    report['source_sha256']={str(p.relative_to(ROOT)):sha(p) for p in sources}
    (ROOT/'reports/native-build.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'ARM64 build exit status: {result.returncode}; see reports/native-game-build.log')
    raise SystemExit(result.returncode)
