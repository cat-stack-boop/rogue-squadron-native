"""Create a private, self-contained development bundle; the game is incomplete."""
from pathlib import Path
import argparse,hashlib,json,plistlib,re,shutil,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
APP=ROOT/'build/Rogue Squadron Development.app'
EXPECTED_ROM='4813551d01d3a3474df3a51f84c31059cd2a9d1eeae7885dda51a88ee6b9f88d'
def run(*args):return subprocess.check_output(args,text=True).strip()
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def system_library(path):return path.startswith(('/System/Library/','/usr/lib/'))
def dependencies(path):
    return [line.strip().split(' (',1)[0] for line in run('otool','-L',str(path)).splitlines()[1:]]
def library_id(path):
    result=run('otool','-D',str(path)).splitlines()
    return result[1].strip() if len(result)>1 else None

def package(result_fixture=False):
    APP=ROOT/'build'/('Rogue Squadron Result Fixture.app' if result_fixture else 'Rogue Squadron Development.app')
    rom=ROOT/'roms/rogue_squadron.us.rev1.z64'
    if sha(rom)!=EXPECTED_ROM:raise ValueError('Expected the verified USA Rev 1 cartridge')
    build=json.loads((ROOT/'reports/native-build.json').read_text())
    if not build['build_succeeded']:raise ValueError('Native build did not succeed')
    for relative,expected in build['source_sha256'].items():
        source=ROOT/relative
        if not source.is_file() or sha(source)!=expected:
            raise ValueError('Native source changed since build: '+relative)
    executable=ROOT/'build/game'/('rogue_result_fixture' if result_fixture else 'rogue_startup')
    renderer=ROOT/'build/gliden64/plugin/Release/mupen64plus-video-GLideN64.dylib'
    for path in (executable,renderer):
        if sha(path)!=build['artifacts'][str(path.relative_to(ROOT))]['sha256']:
            raise ValueError('Native build report is stale: '+str(path))
    with tempfile.TemporaryDirectory(prefix='rogue-package-',dir=ROOT/'build') as folder:
        stage=Path(folder)/APP.name;contents=stage/'Contents'
        mac=contents/'MacOS';frameworks=contents/'Frameworks';resources=contents/'Resources'
        for p in (mac,frameworks,resources):p.mkdir(parents=True,exist_ok=True)
        targets={executable.resolve():mac/'RogueSquadron',renderer.resolve():frameworks/'GLideN64.dylib'}
        pending=list(targets);original_deps={};ids={}
        while pending:
            source=pending.pop(0);ids[source]=library_id(source)
            original_deps[source]=dependencies(source)
            for dep in original_deps[source]:
                if dep==ids[source] or system_library(dep):continue
                if not dep.startswith('/') or not Path(dep).is_file():
                    raise ValueError('Unresolved native dependency: '+dep)
                resolved=Path(dep).resolve()
                if resolved not in targets:
                    target=frameworks/resolved.name
                    if target in targets.values():raise ValueError('Conflicting library names')
                    targets[resolved]=target;pending.append(resolved)
        for source,target in targets.items():
            arch=run('lipo','-archs',str(source)).split()
            if 'arm64' not in arch:raise ValueError('No ARM64 slice: '+str(source))
            if len(arch)>1:subprocess.run(['lipo',str(source),'-thin','arm64','-output',str(target)],check=True)
            else:shutil.copy2(source,target)
            args=['install_name_tool']
            if ids[source]:args+=['-id','@rpath/'+target.name]
            for dep in original_deps[source]:
                if dep==ids[source] or system_library(dep):continue
                dest=targets[Path(dep).resolve()]
                prefix='@executable_path/../Frameworks/' if source==executable.resolve() else '@loader_path/'
                args+=['-change',dep,prefix+dest.name]
            for rpath in re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset',run('otool','-l',str(target))):
                if rpath.startswith('/'):args+=['-delete_rpath',rpath]
            if len(args)>1:subprocess.run(args+[str(target)],check=True)
        shutil.copy2(rom,resources/rom.name)
        shutil.copytree(ROOT/'vendor/GLideN64/ini',resources/'gliden64')
        licenses=resources/'Licenses';licenses.mkdir()
        for source,name in [(ROOT/'vendor/GLideN64/LICENSE','GLideN64.txt'),
                            (ROOT/'vendor/N64ModernRuntime/COPYING','N64ModernRuntime.txt'),
                            (ROOT/'vendor/N64Recomp/LICENSE','N64Recomp.txt'),
                            (ROOT/'vendor/rsp-hle/LICENSES','rsp-hle.txt'),
                            (Path('/opt/homebrew/opt/sdl2/LICENSE.txt'),'SDL2.txt')]:
            shutil.copy2(source,licenses/name)
        shutil.copytree(ROOT/'vendor/GLideN64/licenses',licenses/'GLideN64-components')
        shutil.copy2(ROOT/'config/source-pins.json',resources/'source-pins.json')
        shutil.copy2(ROOT/'docs/CONTROLS.md',resources/'CONTROLS.md')
        shutil.copy2(ROOT/'docs/KNOWN_LIMITATIONS.md',resources/'KNOWN_LIMITATIONS.md')
        shutil.copytree(ROOT/'patches',resources/'Patches')
        (resources/'build-info.json').write_text(json.dumps({'development_build':True,'campaign_fully_validated':False,
            'result_fixture':result_fixture,'architecture':'arm64','rom_sha256':EXPECTED_ROM,'source_sha256':build['source_sha256']},indent=2)+'\n')
        (resources/'README.txt').write_text('Rogue Squadron native ARM64 development build.\n'
            'The port is incomplete; rendering and campaign play still require verification.\n'
            'Saves and configuration: ~/Library/Application Support/Rogue Squadron Native/\n'
            'Keyboard and gamepad controls: CONTROLS.md in this Resources folder.\n'
            'Verified coverage and remaining issues: KNOWN_LIMITATIONS.md.\n'
            'This private bundle contains the locally supplied cartridge.\n'
            'Third-party notices and source revisions are included in Resources.\n')
        info={'CFBundleIdentifier':'local.rogue-squadron-native.result-fixture' if result_fixture else 'local.rogue-squadron-native','CFBundleName':APP.stem,
              'CFBundleExecutable':'RogueSquadron','CFBundlePackageType':'APPL','CFBundleVersion':'0.0.2',
              'CFBundleShortVersionString':'0.0.2','NSHighResolutionCapable':True}
        (contents/'Info.plist').write_bytes(plistlib.dumps(info))
        for target in frameworks.iterdir():subprocess.run(['codesign','--force','--sign','-',str(target)],check=True)
        subprocess.run(['codesign','--force','--sign','-',str(stage)],check=True)
        subprocess.run(['codesign','--verify','--deep','--strict',str(stage)],check=True)
        for target in targets.values():
            for dep in dependencies(target):
                if not system_library(dep) and not dep.startswith(('@rpath/','@loader_path/','@executable_path/')):
                    raise ValueError('External library reference remains: '+dep)
        if APP.is_symlink():raise ValueError('Refusing to replace a symlinked output bundle')
        if APP.exists():shutil.rmtree(APP)
        shutil.move(stage,APP)
    report={'bundle':str(APP),'result_fixture':result_fixture,'architecture':'arm64','signature_verified':True,'campaign_fully_validated':False,
            'rom_sha256':EXPECTED_ROM,'libraries':{},
            'files':{str(p.relative_to(APP)):sha(p) for p in sorted(APP.rglob('*')) if p.is_file()}}
    for p in [APP/'Contents/MacOS/RogueSquadron',*(APP/'Contents/Frameworks').iterdir()]:
        report['libraries'][str(p.relative_to(APP))]={'architecture':run('lipo','-archs',str(p)),
                                                   'dependencies':dependencies(p)}
    (ROOT/'reports'/('result-fixture-package.json' if result_fixture else 'app-package.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(APP)
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--result-fixture',action='store_true')
    package(parser.parse_args().result_fixture)
