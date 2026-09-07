"""Generate candidate native code with a fresh, explicit success/failure report."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


if __name__ == '__main__':
    subprocess.run([str(ROOT/'.venv/bin/python'), 'tools/analyze_code.py'], cwd=ROOT, check=True)
    output = ROOT/'build/candidate-generated'
    output.mkdir(exist_ok=True)
    stage = ROOT/'build/candidate-stage'
    stage.mkdir(exist_ok=True)
    # Generate in a staging directory. Preserve unchanged output timestamps so
    # correcting one function boundary does not recompile the whole cartridge.
    for p in stage.iterdir():
        if p.is_file() and p.suffix in ('.c', '.h', '.inl'):
            p.unlink()
    staged_config = (ROOT/'build/candidate.toml').read_text().replace('output_func_path = "candidate-generated"', 'output_func_path = "candidate-stage"')
    (ROOT/'build/candidate-stage.toml').write_text(staged_config)
    result = subprocess.run(['build/n64recomp/N64Recomp', 'build/candidate-stage.toml'], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (ROOT/'reports/candidate-recompile.log').write_text(result.stdout)
    sources = sorted(stage.glob('*.c'))
    if result.returncode == 0:
        names=set()
        for p in stage.iterdir():
            if p.is_file() and p.suffix in ('.c','.h','.inl'):
                names.add(p.name)
                target=output/p.name
                content=p.read_bytes()
                if not target.exists() or target.read_bytes()!=content:
                    target.write_bytes(content)
        for p in output.iterdir():
            if p.is_file() and p.suffix in ('.c','.h','.inl') and p.name not in names:
                p.unlink()
    report = {'code_generation_succeeded': result.returncode == 0,
              'native_game_booted': False, 'native_generated_files': len(sources),
              'unimplemented_platform_services': json.loads((ROOT/'reports/required-host-services.json').read_text()),
              'warnings': [line for line in result.stdout.splitlines() if '[Warn]' in line],
              'config_sha256': hashlib.sha256((ROOT/'build/candidate.toml').read_bytes()).hexdigest(),
              'symbols_sha256': hashlib.sha256((ROOT/'build/candidate-symbols.toml').read_bytes()).hexdigest()}
    (ROOT/'reports/candidate-generation.json').write_text(json.dumps(report, indent=2)+'\n')
    print(result.stdout)
    print(f'Generated {len(sources)} C files; exit status {result.returncode}. This is not gameplay verification.')
    raise SystemExit(result.returncode)
