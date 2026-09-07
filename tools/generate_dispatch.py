"""Map original code addresses to already-compiled native functions per overlay."""
from pathlib import Path
import tomllib

root = Path(__file__).resolve().parents[1]
symbols = tomllib.loads((root/'build/candidate-symbols.toml').read_text())
config = tomllib.loads((root/'build/candidate.toml').read_text())
ignored = set(config['patches']['ignored'])
entries = []
for overlay, section in enumerate(symbols['section']):
    for f in section['functions']:
        if f['name'] not in ignored:
            entries.append((f['vram'], overlay, f['name']))
entries.sort()
(root/'build/dispatch_entries.inc').write_text('\n'.join(
    f'{{0x{addr:08x}u, {overlay}, &{name}, "{name}"}},' for addr, overlay, name in entries)+'\n')
print(f'Prepared {len(entries)} native function addresses across {len(symbols["section"])} code sections')
