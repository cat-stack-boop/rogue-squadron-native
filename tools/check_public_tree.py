"""Check tracked source files for private artifacts and common secret patterns."""
from pathlib import Path
import re,subprocess,sys

ROOT=Path(__file__).resolve().parents[1]
PRIVATE_DIRS={'roms','assets','build','dist','reports','runtime','vendor','.venv','.codex','.agents'}
PRIVATE_SUFFIXES={'.z64','.n64','.v64','.eep','.bin','.elf','.o','.a','.dylib','.zip','.dmg','.log','.pid','.bmp','.png','.jpg','.jpeg','.wav','.mp4'}
PATTERNS={
    'absolute home path':r'/(?:Users|home)/[^/\s]+',
    'private temporary path':r'/private/var/(?:folders|tmp)/',
    'private key':r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    'GitHub token':r'\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})\b',
    'API key':r'\bsk-[A-Za-z0-9_-]{24,}\b',
    'AWS access key':r'\bAKIA[A-Z0-9]{16}\b',
    'private session identifier':r'\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b',
}
def main():
    result=subprocess.run(['git','ls-files','-z'],cwd=ROOT,capture_output=True,check=True)
    names=[n.decode() for n in result.stdout.split(b'\0') if n]
    if not names:raise SystemExit('No tracked files to inspect; stage the intended source files first.')
    if subprocess.run(['git','diff','--quiet'],cwd=ROOT).returncode:
        raise SystemExit('Tracked files have unstaged changes; stage the intended publication before checking.')
    errors=[]
    for name in names:
        relative=Path(name);path=ROOT/relative
        if relative.parts[0] in PRIVATE_DIRS or path.suffix.lower() in PRIVATE_SUFFIXES or any(p.endswith('.app') for p in relative.parts):
            errors.append((name,'private/build artifact'));continue
        if path.is_symlink():errors.append((name,'symlink'));continue
        data=subprocess.check_output(['git','show',':'+name],cwd=ROOT)
        try:text=data.decode('utf-8')
        except UnicodeDecodeError:errors.append((name,'non-text file'));continue
        if b'\0' in data:errors.append((name,'binary data'))
        for label,pattern in PATTERNS.items():
            if re.search(pattern,text,re.I):errors.append((name,label))
        # Required upstream attribution may contain authors' public contacts.
        if relative.parts[0]!='LICENSES' and re.search(r'\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b',text,re.I):
            errors.append((name,'email address outside upstream notices'))
    for name,reason in errors:print(f'{name}: {reason}',file=sys.stderr)
    if errors:raise SystemExit(1)
    print(f'Public-tree check passed: {len(names)} tracked text files; no private artifacts or detected secrets.')

if __name__=='__main__':main()
