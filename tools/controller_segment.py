"""Send one bounded diagnostic input segment, preserving before/after telemetry.

Call from the original Continue pause menu unless --already-running is supplied.
Completion requires the original pause state; separately verify the PAUSED UI.
"""
import argparse,fcntl,json,struct,time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
BUTTONS={'thrust':0x8000,'fire':0x4000,'brake':0x2000,'camera':0x20,'roll':0x10,
         'look':8,'link':4,'secondary':2,'special':1}

def read_json(path):
    # Mission telemetry predates this helper and is not yet atomically replaced.
    for _ in range(10):
        try:return json.loads(path.read_text())
        except (FileNotFoundError,json.JSONDecodeError):time.sleep(.02)
    raise RuntimeError(f'Cannot read {path}')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ms',type=int,required=True)
    p.add_argument('--x',type=float,default=0);p.add_argument('--y',type=float,default=0)
    p.add_argument('--buttons',nargs='*',choices=BUTTONS,default=[])
    p.add_argument('--already-running',action='store_true')
    p.add_argument('--capture',action='store_true',help='Preserve a renderer frame near the segment midpoint')
    a=p.parse_args()
    if not 1<=a.ms<=10000 or not -1<=a.x<=1 or not -1<=a.y<=1:p.error('Use 1..10000 ms and finite axes -1..1')
    launch=read_json(ROOT/'reports/startup-launch.json')
    if not launch.get('test_controller'):p.error('Current diagnostic launch does not enable --test-controller')
    # The runner holds this lock for its child's lifetime. Unlike process
    # enumeration or kill(pid, 0), checking our own workspace lock works in the
    # normal filesystem sandbox and cannot affect an unrelated process.
    with (ROOT/'reports/startup.lock').open('r') as lock:
        try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:pass
        else:p.error('No diagnostic runner is active')
    state=read_json(ROOT/'reports/controller-state.json')
    if state['phase'] not in ('idle','complete','failed'):p.error('Previous controller segment is still active')
    stamp=time.time_ns();mask=0
    for button in a.buttons:mask|=BUTTONS[button]
    before=read_json(ROOT/'reports/mission-stats.json')
    folder=ROOT/'reports/controller-segments';folder.mkdir(exist_ok=True)
    record={'id':stamp,'executable_sha256':launch['executable_sha256'],'pid':launch['pid'],
            'fixture_level_select_enabled':launch['test_level_select'],'command':vars(a),'before':before,
            'pause_verified':False,'mission_success_verified':False}
    destination=folder/f'{stamp}.json'
    destination.write_text(json.dumps(record,indent=2)+'\n')
    temporary=ROOT/'reports/controller-command.txt.tmp'
    temporary.write_text(f'{stamp} {a.ms} {mask} {a.x} {a.y} {int(not a.already_running)}\n')
    temporary.replace(ROOT/'reports/controller-command.txt')
    deadline=time.monotonic()+a.ms/1000+38
    active_observed=None
    active_observed_wall=None
    while time.monotonic()<deadline:
        state=read_json(ROOT/'reports/controller-state.json')
        if state.get('rejected_id')==stamp:
            record['state']=state;destination.write_text(json.dumps(record,indent=2)+'\n')
            raise SystemExit(f'Controller command rejected: check that the original Continue menu is open. Evidence: {destination}')
        if state['id']==stamp and state['phase']=='active':
            if active_observed is None:
                active_observed=time.monotonic();active_observed_wall=time.time_ns()
            if a.capture and 'midpoint_frame' not in record and time.monotonic()-active_observed>=a.ms/2000:
                frame=ROOT/'reports/first-game-frame.bmp'
                try:
                    previous=frame.stat();pixels=frame.read_bytes();current=frame.stat()
                    if (previous.st_mtime_ns==current.st_mtime_ns and current.st_mtime_ns>=active_observed_wall and len(pixels)>=54 and
                        pixels[:2]==b'BM' and struct.unpack_from('<I',pixels,2)[0]==len(pixels)):
                        saved=folder/f'{stamp}-midpoint.bmp';saved.write_bytes(pixels)
                        record.update(midpoint_frame=str(saved),midpoint_frame_mtime_ns=current.st_mtime_ns)
                except FileNotFoundError:pass
        if state['id']==stamp and state['phase'] in ('complete','failed'):break
        time.sleep(.05)
    else:
        raise SystemExit(f'Input sequence timed out; inspect the game. Evidence: {destination}')
    # The original freeze flag precedes the pause menu's entrance animation.
    # Let that menu become ready before a caller immediately sends the next A.
    time.sleep(1.2 if state['phase']=='complete' else .6)
    record.update(state=state,after=read_json(ROOT/'reports/mission-stats.json'))
    destination.write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'report':str(destination),'state':state,'after':record['after']},indent=2))
    if state['phase']=='failed':raise SystemExit(2)
