"""Run the native startup experiment with identity checking and a hard timeout."""
import json,argparse,os,fcntl,hashlib,time
import subprocess
from pathlib import Path
from inspect_rom import inspect

ROOT = Path(__file__).resolve().parents[1]


if __name__ == '__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--seconds',type=int,default=30)
    parser.add_argument('--app',action='store_true')
    parser.add_argument('--test-level-select',action='store_true',help='Enable original level-select cheat flags in isolated campaign-test saves only')
    parser.add_argument('--pause-on-mission',action='store_true',help='Request Start on the first original mission update for scene review')
    parser.add_argument('--test-controller',action='store_true',help='Accept bounded controller test segments in isolated campaign-test saves')
    parser.add_argument('--user-data-dir',type=Path,default=ROOT/'runtime');args=parser.parse_args()
    if not 0<=args.seconds<=3600:raise ValueError('Seconds must be 0 (interactive) to 3600')
    # Diagnostic filenames are shared. A second runner must not truncate a live
    # test's log or overwrite its PID and frame evidence.
    run_lock=(ROOT/'reports/startup.lock').open('a')
    try:fcntl.flock(run_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:raise SystemExit('A native diagnostic runner is already active')
    environment=os.environ.copy();environment['ROGUE_RUN_SECONDS']=str(args.seconds)
    environment['ROGUE_DIAGNOSTICS']='1'
    environment.pop('ROGUE_TEST_LEVEL_SELECT',None)
    environment.pop('ROGUE_PAUSE_ON_MISSION',None)
    environment.pop('ROGUE_TEST_CONTROLLER',None)
    if args.test_controller:
        if not args.user_data_dir.resolve().is_relative_to((ROOT/'runtime/campaign-tests').resolve()):
            raise ValueError('Controller tests require --user-data-dir under runtime/campaign-tests')
        environment['ROGUE_TEST_CONTROLLER']='1'
        (ROOT/'reports/controller-command.txt').unlink(missing_ok=True)
        (ROOT/'reports/controller-state.json').unlink(missing_ok=True)
    if args.pause_on_mission:environment['ROGUE_PAUSE_ON_MISSION']='1'
    if args.test_level_select:
        if not args.user_data_dir.resolve().is_relative_to((ROOT/'runtime/campaign-tests').resolve()):
            raise ValueError('Level-select fixture requires --user-data-dir under runtime/campaign-tests')
        environment['ROGUE_TEST_LEVEL_SELECT']='1'
    # The diagnostic runner deliberately reuses the development saves/reports.
    # Normal bundle launches choose Application Support independently.
    environment['ROGUE_USER_DATA_DIR']=str(args.user_data_dir.resolve())
    environment['ROGUE_DIAGNOSTICS_DIR']=str(ROOT/'reports')
    identity = inspect(ROOT/'roms/rogue_squadron.us.rev1.z64')
    expected = json.loads((ROOT/'config/segments.json').read_text())['rom_sha256']
    if identity['sha256'] != expected:
        raise ValueError('Startup runner does not match this cartridge')
    (ROOT/'reports/startup.json').unlink(missing_ok=True)
    (ROOT/'reports/runtime-counters.json').unlink(missing_ok=True)
    (ROOT/'reports/stopped-context.json').unlink(missing_ok=True)
    (ROOT/'reports/stopped-rdram.bin').unlink(missing_ok=True)
    (ROOT/'reports/audio-timing.json').unlink(missing_ok=True)
    (ROOT/'reports/mission-stats.json').unlink(missing_ok=True)
    (ROOT/'reports/entities.json').unlink(missing_ok=True)
    outer_timeout=False
    with (ROOT/'reports/startup.log').open('w') as log:
        executable='build/Rogue Squadron Development.app/Contents/MacOS/RogueSquadron' if args.app else 'build/game/rogue_startup'
        launch={'executable':str(ROOT/executable),
                'executable_sha256':hashlib.sha256((ROOT/executable).read_bytes()).hexdigest(),
                'started_unix':time.time(),'requested_seconds':args.seconds,
                'test_level_select':args.test_level_select,'pause_on_mission':args.pause_on_mission,'test_controller':args.test_controller,
                'user_data_dir':str(args.user_data_dir.resolve())}
        with subprocess.Popen([str(ROOT/executable)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,env=environment) as process:
            (ROOT/'reports/startup.pid').write_text(str(process.pid)+'\n')
            launch['pid']=process.pid
            (ROOT/'reports/startup-launch.json').write_text(json.dumps(launch,indent=2)+'\n')
            try:
                exit_code=process.wait(timeout=args.seconds+15 if args.seconds else None)
            except subprocess.TimeoutExpired:
                process.kill();process.wait();outer_timeout=True;exit_code=124
    lines=(ROOT/'reports/startup.log').read_text().splitlines()
    if len(lines)>45: print(f'Full trace saved in reports/startup.log ({len(lines)} lines)')
    print('\n'.join(lines[-45:]))
    report = ROOT/'reports/startup.json'
    if outer_timeout:
        report.write_text(json.dumps({'game_booted':False,'reason':'outer_process_timeout','child_killed_and_reaped':True})+'\n')
    elif not report.exists():
        report.write_text(json.dumps({'game_booted':False,
            'reason':'process_signal' if exit_code<0 else 'exit_without_runtime_report',
            'signal':-exit_code if exit_code<0 else None,'child_reaped':True})+'\n')
    if report.exists():
        evidence = json.loads(report.read_text())
        counters=ROOT/'reports/runtime-counters.json'
        if counters.exists():evidence.update(json.loads(counters.read_text()))
        evidence['rom_sha256'] = identity['sha256']
        evidence['exit_code'] = exit_code
        evidence['launch'] = launch
        report.write_text(json.dumps(evidence, indent=2)+'\n')
        print(report.read_text())
    raise SystemExit(exit_code)
