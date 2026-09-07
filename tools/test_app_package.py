"""Run a relocated bundle with source/Homebrew reads and bundle writes denied."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,struct,subprocess,tempfile,time,zlib
ROOT=Path(__file__).resolve().parents[1]

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def files(folder):return {str(p.relative_to(folder)):sha(p) for p in folder.rglob('*') if p.is_file()}
def checksums(path):
    data=path.read_bytes()
    return len(data)==512 and all(struct.unpack_from('>I',data,o)[0]==zlib.adler32(data[s:s+n],1)
        for o,s,n in [(0,4,12),(16,20,12),(0x24,0x28,16),(0x20,0x38,176),(0xec,0xf0,16),(0xe8,0x100,176)])

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--seconds',type=int,default=45)
    parser.add_argument('--normal-launch',action='store_true')
    parser.add_argument('--audio-unavailable',action='store_true',help='Verify startup and timing with no usable SDL audio driver')
    parser.add_argument('--recover-save',action='store_true',help='Verify startup from a truncated disposable save and valid backup')
    args=parser.parse_args()
    if args.audio_unavailable and not args.normal_launch:raise ValueError('Audio-unavailable verification requires --normal-launch')
    if args.recover_save and (not args.normal_launch or args.audio_unavailable):raise ValueError('Save recovery requires normal launch with normal audio')
    if not 10<=args.seconds<=180:raise ValueError('Expected 10 to 180 seconds')
    if args.normal_launch and args.seconds<35:raise ValueError('Normal-launch test must exceed the former 30-second watchdog')
    report={'passed':False,'normal_launch':args.normal_launch,'audio_unavailable':args.audio_unavailable,'recover_save':args.recover_save};original_save=ROOT/'runtime/saves/rogue-squadron.eep'
    original_save_hash=sha(original_save) if original_save.exists() else None
    normal_save=Path.home()/'Library/Application Support/Rogue Squadron Native/saves/rogue-squadron.eep'
    normal_hash=sha(normal_save) if normal_save.exists() else None
    folder=Path(tempfile.mkdtemp(prefix='rogue-portability-'))
    report['test_directory']=str(folder)
    try:
        app=folder/'Relocated Rogue Squadron.app'
        shutil.copytree(ROOT/'build/Rogue Squadron Development.app',app)
        before=files(app);exe=app/'Contents/MacOS/RogueSquadron'
        print(json.dumps({'test_app':str(app),'test_executable':str(exe),'test_directory':str(folder)}),flush=True)
        profile=folder/'isolation.sb'
        profile.write_text('(version 1)\n(allow default)\n(deny file-read* (subpath '+json.dumps(str(ROOT))+') (subpath "/opt/homebrew"))\n'
                           '(deny file-write* (subpath '+json.dumps(str(app))+'))\n')
        denied={}
        for name,path in [('checkout',ROOT/'roms/rogue_squadron.us.rev1.z64'),('homebrew',Path('/opt/homebrew/opt/sdl2/LICENSE.txt'))]:
            result=subprocess.run(['sandbox-exec','-f',str(profile),'/bin/cat',str(path)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
            denied[name]=result.returncode!=0 and 'Operation not permitted' in result.stderr
        if not all(denied.values()):raise AssertionError('The negative controls did not prove read isolation')
        report['read_denial_controls']=denied
        diagnostic=folder/'diagnostics';diagnostic.mkdir()
        if args.recover_save:
            if not normal_save.exists() or not checksums(normal_save):raise ValueError('Save recovery needs an existing checksum-valid normal save')
            saves=folder/'user-data/saves';saves.mkdir(parents=True)
            recovery_fixture=normal_save.read_bytes();damaged=recovery_fixture[:111]
            (saves/'rogue-squadron.eep').write_bytes(damaged)
            (saves/'rogue-squadron.previous.eep').write_bytes(recovery_fixture)
        environment={key:value for key,value in os.environ.items() if not key.startswith('ROGUE_')}
        environment.pop('SDL_VIDEO_MAC_FULLSCREEN_SPACES',None)
        environment.pop('SDL_AUDIODRIVER',None)
        if args.audio_unavailable:environment['SDL_AUDIODRIVER']='rogue_test_unavailable_driver'
        environment.update(ROGUE_USER_DATA_DIR=str(folder/'user-data'),
            ROGUE_DIAGNOSTICS_DIR=str(diagnostic),ROGUE_RUN_SECONDS=str(args.seconds))
        if args.normal_launch:
            environment.pop('ROGUE_RUN_SECONDS',None);environment.pop('ROGUE_DIAGNOSTICS',None)
        else:environment['ROGUE_DIAGNOSTICS']='1'
        command=['sandbox-exec','-f',str(profile),str(exe)]
        with (diagnostic/'startup.log').open('w') as log,subprocess.Popen(command,cwd=folder,env=environment,stdout=log,stderr=subprocess.STDOUT) as process:
            (ROOT/'reports/portable-test.pid').write_text(str(process.pid)+'\n')
            for _ in range(80):
                if (diagnostic/'runtime-paths.json').exists() or process.poll() is not None:break
                time.sleep(0.05)
            if process.poll() is not None:raise AssertionError('Relocated app exited before startup')
            duplicate=subprocess.run(command,cwd=folder,env=environment,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=10)
            report['duplicate_instance_rejected']=duplicate.returncode==2 and 'Another game instance' in duplicate.stdout
            if args.normal_launch:
                try:
                    process.wait(timeout=args.seconds)
                    raise AssertionError('Normal app launch ended before the test duration')
                except subprocess.TimeoutExpired:
                    report['normal_launch_remained_running_seconds']=args.seconds
                    process.terminate()
                    try:process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill();process.wait();raise AssertionError('Normal app did not close after termination')
            else:
                try:process.wait(timeout=args.seconds+15)
                except subprocess.TimeoutExpired:
                    process.kill();process.wait();raise AssertionError('Relocated test exceeded its outer timeout')
        state=json.loads((diagnostic/'startup.json').read_text())
        counters=json.loads((diagnostic/'runtime-counters.json').read_text())
        paths=json.loads((diagnostic/'runtime-paths.json').read_text())
        report['runtime']=state|counters;report['paths']=paths
        save_load=json.loads((diagnostic/'save-load.json').read_text());report['save_load']=save_load
        if args.recover_save:
            assert save_load['source']=='backup'
            preserved=Path(save_load['preserved_file'])
            assert preserved.parent==folder/'user-data/saves' and preserved.read_bytes()==damaged
            assert (folder/'user-data/saves/rogue-squadron.previous.eep').read_bytes()==recovery_fixture
            assert (folder/'user-data/saves/rogue-squadron.eep').read_bytes()==recovery_fixture
            report['damaged_save_preserved']=True;report['restored_save_matches_fixture']=True
        audio=json.loads((diagnostic/'audio-timing.json').read_text())
        report['audio']=audio
        phases=audio['phases'];assert phases and counters['audio_tasks']>0
        if args.audio_unavailable:
            assert all(p['device_open_attempts']==p['device_open_failures']>0 for p in phases)
            assert sum(p['offline_frames'] for p in phases)==sum(p['frames_queued'] for p in phases)>0
            assert sum(p['dma_completions'] for p in phases)>100
            assert sum(p['device_open_attempts'] for p in phases)<args.seconds*2
        else:
            assert phases[-1]['device_open_attempts']>phases[-1]['device_open_failures']
            assert phases[-1]['device_period_frames']>0
            assert sum(p['offline_frames'] for p in phases)<sum(p['frames_queued'] for p in phases)
        window=json.loads((diagnostic/'window-state.json').read_text())
        renderer=json.loads((diagnostic/'renderer-size.json').read_text())
        report['window']=window;report['renderer']=renderer
        assert (renderer['width'],renderer['height'])==(window['drawable_width'],window['drawable_height'])
        report['bundle_unchanged']=files(app)==before
        report['separate_save_valid']=checksums(folder/'user-data/saves/rogue-squadron.eep')
        report['development_save_unchanged']=(sha(original_save) if original_save.exists() else None)==original_save_hash
        report['normal_save_unchanged']=(sha(normal_save) if normal_save.exists() else None)==normal_hash
        assert state['reason']==('window_closed' if args.normal_launch else 'startup_timeout') and state['graphics_tasks']>100
        if args.normal_launch:
            assert process.returncode==0
            assert not paths['diagnostics'] and not (diagnostic/'first-game-frame.bmp').exists() and not (diagnostic/'stopped-rdram.bin').exists()
            trace=(diagnostic/'runtime.log').read_text()
            assert 'GPU HLE completed:' not in trace and 'Native event queue: submit' not in trace
            report['normal_log_bytes']=len(trace.encode())
        assert paths['bundled'] and paths['rom']==str(app/'Contents/Resources/rogue_squadron.us.rev1.z64')
        assert paths['renderer']==str(app/'Contents/Frameworks/GLideN64.dylib')
        for key in ['bundle_unchanged','separate_save_valid','development_save_unchanged','normal_save_unchanged','duplicate_instance_rejected']:assert report[key],key
        subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
        suffix='-save-recovery' if args.recover_save else '-audio-unavailable' if args.audio_unavailable else ''
        evidence=ROOT/'reports'/('portable-app'+suffix);evidence.mkdir(exist_ok=True)
        for name in ['startup.log','startup.json','runtime-counters.json','runtime-paths.json','graphics-context.txt','window-state.json','renderer-size.json','audio-timing.json','save-load.json']:
            shutil.copy2(diagnostic/name,evidence/name)
        if (diagnostic/'runtime.log').exists():shutil.copy2(diagnostic/'runtime.log',evidence/'runtime.log')
        # Independently corrupt only the disposable copied cartridge and resign
        # the copied bundle. The native ROM guard must reject it before graphics.
        rom=app/'Contents/Resources/rogue_squadron.us.rev1.z64'
        with rom.open('r+b') as data:data.seek(0x1000);value=data.read(1)[0];data.seek(0x1000);data.write(bytes([value^1]))
        subprocess.run(['codesign','--force','--sign','-',str(app)],check=True)
        negative=folder/'negative-reports'
        environment.update(ROGUE_USER_DATA_DIR=str(folder/'negative-user-data'),ROGUE_DIAGNOSTICS_DIR=str(negative))
        bad=subprocess.run(command,cwd=folder,env=environment,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=10)
        bad_state=json.loads((negative/'startup.json').read_text())
        report['wrong_cartridge_rejected']=bad.returncode==3 and bad_state['reason']=='cartridge_identity_mismatch' and bad_state['native_calls']==0
        assert report['wrong_cartridge_rejected']
        report['passed']=True
    except Exception as error:
        report['error']=str(error);raise
    finally:
        suffix='-save-recovery' if args.recover_save else '-audio-unavailable' if args.audio_unavailable else ''
        report_path=ROOT/'reports'/('app-portability'+suffix+'.json')
        report_path.write_text(json.dumps(report,indent=2)+'\n')
        if report['passed']:shutil.rmtree(folder)
        print(json.dumps(report,indent=2))

if __name__=='__main__':main()
