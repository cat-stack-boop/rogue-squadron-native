"""Bounded production audio-clock/output regressions with SDL dummy devices."""
from pathlib import Path
import hashlib,json,subprocess

ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    report={'passed':False,'physical_device_faults_verified':False,'tests':{}}
    try:
        for name,binary,timing in [
            ('buffer_recovery','rogue_audio_recovery_probe','audio-recovery-timing.json'),
            ('device_recovery','rogue_audio_device_probe','audio-device-timing.json')]:
            executable=ROOT/'build/game'/binary
            result=subprocess.run([str(executable),str(ROOT/'reports'/timing)],cwd=ROOT,
                text=True,capture_output=True,timeout=15)
            (ROOT/'reports'/f'{name}.log').write_text(result.stdout+result.stderr)
            if result.returncode:raise RuntimeError(f'{binary} failed: {result.stderr}')
            outcome=json.loads(result.stdout.splitlines()[-1])
            assert outcome['passed']
            report['tests'][name]=outcome|{'executable_sha256':sha(executable),'timing':timing}
            print(f'{name}: passed ({outcome["dma_completions"]} DMA completions)')
        phases=json.loads((ROOT/'reports/audio-device-timing.json').read_text())['phases']
        assert sum(p['device_open_failures'] for p in phases)==3
        assert sum(p['queue_failures'] for p in phases)==1
        assert sum(p['stalled_devices'] for p in phases)==1
        assert sum(p['device_recoveries'] for p in phases)==4
        assert sum(p['offline_frames'] for p in phases)>0
        result=report['tests']['device_recovery']
        assert result['dma_completions']==result['submitted_buffers']
        report['source_sha256']={name:sha(ROOT/name) for name in [
            'src/native_audio.cpp','src/native_audio.hpp','src/native_audio_device.cpp',
            'src/native_audio_device.hpp','src/audio_device_probe.cpp','src/audio_recovery_probe.cpp',
            'src/audio_dma_clock.hpp']}
        report['passed']=True
    finally:
        (ROOT/'reports/audio-device-verification.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
