"""Bounded UI integration run of synthetic success through original game code."""
from pathlib import Path
import argparse,hashlib,json,os,struct,subprocess,tempfile,time,zlib

ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def valid(data):return len(data)==512 and all(struct.unpack_from('>I',data,o)[0]==zlib.adler32(data[s:s+n])
    for o,s,n in [(0,4,12),(16,20,12),(0x24,0x28,16),(0x20,0x38,176),(0xec,0xf0,16),(0xe8,0x100,176)])

def verify_result(report):
    reason=report.get('runtime',{}).get('reason')
    controlled_stop=(report.get('exit_code')==0 and reason=='window_closed') or (report.get('exit_code')==3 and reason=='startup_timeout')
    fixture=report.get('fixture',{})
    # The dedicated runtime's bounded watchdog uses status 3 by design. It is
    # acceptable only after the original result function returned and both
    # complete records contain the expected advancement/medal.
    return controlled_stop and fixture.get('phase')=='results_closed' and fixture.get('synthetic_score',False) and report.get('save_valid',False) and report.get('stored_max_levels')==[1,1] and report.get('stored_first_medals')==[3,3] and report.get('normal_save_unchanged',False) and report.get('seed_unchanged',False)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--seconds',type=int,default=240)
    parser.add_argument('--seed-save',type=Path,default=Path.home()/'Library/Application Support/Rogue Squadron Native/saves/rogue-squadron.eep')
    args=parser.parse_args();assert 60<=args.seconds<=240
    original=args.seed_save.resolve();seed=original.read_bytes();seed_hash=sha(original)
    assert valid(seed) and seed[0x51]==0 and seed[0x119]==0, 'Fixture requires a valid first-mission save'
    normal=Path.home()/'Library/Application Support/Rogue Squadron Native/saves/rogue-squadron.eep'
    normal_hash=sha(normal)
    parent=ROOT/'runtime/campaign-tests';parent.mkdir(exist_ok=True)
    folder=Path(tempfile.mkdtemp(prefix='result-',dir=parent));(folder/'saves').mkdir()
    (folder/'.result-fixture').write_text('Synthetic completion fixture; no real player achievements\n')
    save=folder/'saves/rogue-squadron.eep';save.write_bytes(seed)
    reports=ROOT/'reports'/folder.name;reports.mkdir()
    app=ROOT/'build/Rogue Squadron Result Fixture.app';exe=app/'Contents/MacOS/RogueSquadron'
    package=json.loads((ROOT/'reports/result-fixture-package.json').read_text())
    assert package['result_fixture'] and sha(exe)==package['files']['Contents/MacOS/RogueSquadron']
    environment={k:v for k,v in os.environ.items() if not k.startswith('ROGUE_')}
    # Prove the compiled fixture rejects normal/default save paths before any
    # normal Application Support state can be created or modified.
    rejected=subprocess.run([str(exe)],env=environment,capture_output=True,text=True,timeout=5)
    assert rejected.returncode==2 and 'marked isolated' in rejected.stderr
    environment.update(ROGUE_USER_DATA_DIR=str(folder),ROGUE_DIAGNOSTICS_DIR=str(reports),
        ROGUE_DIAGNOSTICS='0',ROGUE_RUN_SECONDS=str(args.seconds),ROGUE_RESULT_FIXTURE_MODE='success_gold')
    report={'passed':False,'synthetic_completion':True,'legitimate_played_win':False,'app':str(app),
            'executable_sha256':sha(exe),'user_data':str(folder),'reports':str(reports),'seed_sha256':seed_hash,
            'unsafe_launch_rejected':True,'started_unix':time.time(),'requested_seconds':args.seconds}
    with (reports/'stdout.log').open('w') as log,subprocess.Popen([str(exe)],env=environment,stdout=log,stderr=subprocess.STDOUT) as process:
        report['pid']=process.pid
        (ROOT/'reports/result-fixture-last.json').write_text(json.dumps(report,indent=2)+'\n')
        (reports/'launch.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
        try:report['exit_code']=process.wait(timeout=args.seconds+15)
        except subprocess.TimeoutExpired:
            process.kill();process.wait();report['exit_code']=124;report['outer_timeout']=True
    if (reports/'startup.json').exists():report['runtime']=json.loads((reports/'startup.json').read_text())
    if (reports/'result-fixture.json').exists():report['fixture']=json.loads((reports/'result-fixture.json').read_text())
    data=save.read_bytes();report['save_valid']=valid(data)
    report['stored_max_levels']=[data[0x51],data[0x119]]
    report['stored_first_medals']=[data[0x52]&3,data[0x11a]&3]
    report['normal_save_unchanged']=sha(normal)==normal_hash
    report['seed_unchanged']=sha(original)==seed_hash
    report['passed']=verify_result(report)
    (reports/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    (ROOT/'reports/result-fixture-last.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    raise SystemExit(0 if report['passed'] else 1)

if __name__=='__main__':main()
