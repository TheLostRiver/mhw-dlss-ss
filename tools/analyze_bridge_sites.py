"""Targeted offline inspection of the user-supplied MHWSS; never loads it."""
import bisect
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / '.packages'))
import capstone
import pefile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--evidence',type=Path,default=root/'evidence/mhwss-pe.json')
parser.add_argument('--output',type=Path,default=root/'evidence/mhwss-bridge-sites')
options=parser.parse_args()
meta = json.loads(options.evidence.read_text('utf-8-sig'))
for destination in (options.output.with_suffix('.asm'),options.output.with_suffix('.json')):
    if destination.resolve() in (options.evidence.resolve(),Path(meta['path']).resolve()):
        parser.error('Outputs must not overwrite input evidence or the inspected binary.')
data = Path(meta['path']).read_bytes()
assert hashlib.sha256(data).hexdigest() == meta['sha256']
pe = pefile.PE(data=data)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.skipdata = True
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [a for a, b in funcs]
strings = {int(s['rva'], 16): s['text'] for s in meta['strings'] if s['rva']}
targets = {0xefb90, 0xed790, 0x54ebe3, 0x559b08, 0x559b18, 0x559b20, 0x559b28}
selected = {0xef180, 0xed790, 0xeefe0, 0x300bf0, 0x300e10}
refs = []
rip = re.compile(r'\[rip ([+-]) (0x[0-9a-f]+)\]')
for sec in pe.sections:
    if not sec.Characteristics & 0x20000000:
        continue
    for addr, size, op, args in md.disasm_lite(sec.get_data(), sec.VirtualAddress):
        m = rip.search(args)
        target = addr + size + int(m[2], 16) * (1 if m[1] == '+' else -1) if m else None
        if op == 'call' and args.startswith('0x'):
            target = int(args, 16)
        if target in targets:
            owner = funcs[bisect.bisect_right(starts, addr) - 1]
            refs.append({'rva':hex(addr),'target':hex(target),'owner':hex(owner[0]),'instruction':op+' '+args})
            selected.add(owner[0])
lines = []
for entry in sorted(selected):
    start, end = funcs[bisect.bisect_right(starts, entry) - 1]
    lines.append(f'FUNCTION {start:#x}..{end:#x}')
    for addr, size, op, args in md.disasm_lite(pe.get_data(start, end-start), start):
        m = rip.search(args)
        target = addr + size + int(m[2],16) * (1 if m[1]=='+' else -1) if m else None
        note = f' ; {target:#x} {strings.get(target, "")}' if target else ''
        lines.append(f'{addr:08x} {op:9} {args}{note}')
    lines.append('')
vtrefs = []
for value in (0xefb90, 0xed790):
    needle = struct.pack('<Q',pe.OPTIONAL_HEADER.ImageBase+value)
    pos = -1
    while (pos := data.find(needle,pos+1)) >= 0:
        vtrefs.append({'function':hex(value),'pointer_rva':hex(pe.get_rva_from_offset(pos))})
out = options.output
out.parent.mkdir(parents=True,exist_ok=True)
out.with_suffix('.asm').write_text('\n'.join(lines),'utf-8')
report={'binary_sha256':meta['sha256'],'references':refs,'pointer_references':vtrefs}
out.with_suffix('.json').write_text(json.dumps(report,indent=2),'utf-8')
print(json.dumps(report,indent=2))
