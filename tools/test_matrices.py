"""Build and run independent checks of the original native matrix routines."""
import hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
subprocess.run(['clang','-arch','arm64','-std=c17','-O2','-fno-strict-aliasing','-fwrapv',
    '-Ivendor/N64Recomp/include','-Ibuild/candidate-generated','src/matrix_probe.c',
    'build/game/librogue_game.a','-o','build/matrix_probe'],cwd=root,check=True)
result=json.loads(subprocess.check_output(['build/matrix_probe'],cwd=root,text=True))
result['source_sha256']=hashlib.sha256((root/'src/matrix_probe.c').read_bytes()).hexdigest()
result['cpu_status_sha256']=hashlib.sha256((root/'src/cpu_status.h').read_bytes()).hexdigest()
result['game_library_sha256']=hashlib.sha256((root/'build/game/librogue_game.a').read_bytes()).hexdigest()
(root/'reports/matrix-probe.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
