"""Fetch pinned public tools and build them in the project; never alters ROMs."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
(ROOT/'reports').mkdir(exist_ok=True)
(ROOT/'build').mkdir(exist_ok=True)


def run(*args, cwd=ROOT):
    subprocess.run(args, cwd=cwd, check=True)


if __name__ == '__main__':
    pins = json.loads((ROOT/'config/source-pins.json').read_text())
    urls = {'rogue_squadron64': 'https://github.com/Tmcg2/rogue_squadron64.git',
            'N64Recomp': 'https://github.com/N64Recomp/N64Recomp.git',
            'N64ModernRuntime': 'https://github.com/N64Recomp/N64ModernRuntime.git',
            'GLideN64': 'https://github.com/gonetz/GLideN64.git',
            'rsp-hle': 'https://github.com/mupen64plus/mupen64plus-rsp-hle.git'}
    if not (ROOT/'.venv/bin/python').exists():
        run(sys.executable, '-m', 'venv', '.venv')
    run('.venv/bin/python', '-m', 'pip', 'install', '--only-binary=:all:', '-r', 'requirements-tools.txt')
    for name, url in urls.items():
        path = ROOT/'vendor'/name
        if not path.exists():
            run('git', 'clone', '--no-checkout', '--filter=blob:none', url, str(path))
            run('git', 'checkout', '--detach', pins[name], cwd=path)
        head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=path, text=True).strip()
        if head != pins[name]:
            raise RuntimeError(f'{name} has a different checkout; preserving it instead of resetting')
    run('git', 'submodule', 'update', '--init', '--recursive', cwd=ROOT/'vendor/N64Recomp')
    patch = ROOT/'patches/n64recomp-cli-function-lookup.patch'
    applied = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)],
                             cwd=ROOT/'vendor/N64Recomp', capture_output=True).returncode == 0
    if not applied:
        run('git', 'apply', '--check', str(patch), cwd=ROOT/'vendor/N64Recomp')
        run('git', 'apply', str(patch), cwd=ROOT/'vendor/N64Recomp')
    runtime_patch=ROOT/'patches/runtime-sp-completion-after-parse.patch'
    runtime_repo=ROOT/'vendor/N64ModernRuntime'
    if subprocess.run(['git','apply','--reverse','--check',str(runtime_patch)],cwd=runtime_repo,capture_output=True).returncode!=0:
        run('git','apply','--check',str(runtime_patch),cwd=runtime_repo)
        run('git','apply',str(runtime_patch),cwd=runtime_repo)
    gfx_patch=ROOT/'patches/gliden64-frame-diagnostics.patch'
    gfx_repo=ROOT/'vendor/GLideN64'
    if subprocess.run(['git','apply','--reverse','--check',str(gfx_patch)],cwd=gfx_repo,capture_output=True).returncode!=0:
        run('git','apply','--check',str(gfx_patch),cwd=gfx_repo)
        run('git','apply',str(gfx_patch),cwd=gfx_repo)
    rsp_patch=ROOT/'patches/rsp-hle-voice-bounds.patch'
    rsp_repo=ROOT/'vendor/rsp-hle'
    if subprocess.run(['git','apply','--reverse','--check',str(rsp_patch)],cwd=rsp_repo,capture_output=True).returncode!=0:
        run('git','apply','--check',str(rsp_patch),cwd=rsp_repo)
        run('git','apply',str(rsp_patch),cwd=rsp_repo)
    for source, output in [('vendor/N64Recomp', 'build/n64recomp'),
                           ('vendor/N64ModernRuntime/ultramodern', 'build/ultramodern')]:
        run('.venv/bin/cmake', '-S', source, '-B', output, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_MAKE_PROGRAM={ROOT}/.venv/bin/ninja',
            '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DCMAKE_OSX_ARCHITECTURES=arm64')
        targets = ['--target', 'N64RecompCLI', 'RSPRecomp'] if 'n64recomp' in output else []
        run('.venv/bin/cmake', '--build', output, *targets, '-j', '4')
    run('.venv/bin/cmake','-S','vendor/GLideN64/src','-B','build/gliden64','-G','Ninja',
        '-DMUPENPLUSAPI=ON','-DNOHQ=ON','-DNO_OSD=ON','-DUSE_IPO=OFF',
        '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OSX_ARCHITECTURES=arm64',
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',f'-DCMAKE_MAKE_PROGRAM={ROOT}/.venv/bin/ninja',
        '-DCMAKE_PREFIX_PATH=/opt/homebrew')
    run('.venv/bin/cmake','--build','build/gliden64','-j','4')
