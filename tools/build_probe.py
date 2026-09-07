"""Generate native code from the exact inspected ROM and run functional checks."""
import json
import hashlib
import subprocess
from pathlib import Path
from inspect_rom import inspect

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run(args, check=True, cwd=ROOT)


if __name__ == '__main__':
    (ROOT / 'reports/native-probe.json').unlink(missing_ok=True)
    identity = inspect(ROOT / 'roms/rogue_squadron.us.rev1.z64')
    if identity['sha256'] != '4813551d01d3a3474df3a51f84c31059cd2a9d1eeae7885dda51a88ee6b9f88d':
        raise ValueError('The reviewed symbol map only applies to the supplied Revision 1 ROM')
    run('build/n64recomp/N64Recomp', 'config/probe.toml')
    generated = sorted((ROOT / 'build/probe-generated').glob('*.c'))
    run('clang', '-arch', 'arm64', '-std=c17', '-O2', '-fno-strict-aliasing', '-fwrapv',
        '-Ivendor/N64Recomp/include', '-Ibuild/probe-generated', '-include', 'src/probe_services.h',
        'src/native_probe.c', *(str(p) for p in generated), '-lz', '-o', 'build/native_probe')
    result = subprocess.check_output(['build/native_probe'], cwd=ROOT, text=True)
    report = json.loads(result)
    report['rom_sha256'] = identity['sha256']
    report['source_sha256'] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [ROOT/'src/native_probe.c', ROOT/'src/probe_services.h', ROOT/'config/probe.toml', ROOT/'config/probe-symbols.toml']}
    report['binary_file_type'] = subprocess.check_output(['file', 'build/native_probe'], cwd=ROOT, text=True).strip()
    (ROOT / 'reports/native-probe.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
