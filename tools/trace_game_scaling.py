"""Read-only live .text snapshot and targeted string xrefs for the inspected MHW build."""
import argparse
import bisect
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / ".packages"))
import capstone
import pefile

EXPECTED = "c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pid", type=int)
parser.add_argument("modules", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--functions", nargs="*", type=lambda x: int(x, 0), default=[])
args = parser.parse_args()
mods = json.loads(args.modules.read_text(encoding="utf-8-sig"))
game = next(m for m in mods if m["name"].lower() == "monsterhunterworld.exe")
binary = Path(game["path"])
disk = binary.read_bytes()
if hashlib.sha256(disk).hexdigest() != EXPECTED:
    raise ValueError("Unexpected game build")
pe = pefile.PE(data=disk, fast_load=True)
section = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
args.output.mkdir(parents=True, exist_ok=True)
snapshot = args.output / "text.bin"
manifest_path = args.output / "snapshot.json"
if snapshot.exists():
    meta = json.loads(manifest_path.read_text(encoding="utf-8"))
    code = snapshot.read_bytes()
    if meta["pid"] != args.pid or meta["game_sha256"] != EXPECTED or hashlib.sha256(code).hexdigest() != meta["text_sha256"]:
        raise ValueError("Snapshot identity mismatch")
else:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel.ReadProcessMemory.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x410, False, args.pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        path = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(length)) or Path(path.value).resolve() != binary.resolve():
            raise ValueError("Process image mismatch")
        pieces = []
        for offset in range(0, section.Misc_VirtualSize, 1024 * 1024):
            size = min(1024 * 1024, section.Misc_VirtualSize - offset)
            buffer, count = ctypes.create_string_buffer(size), ctypes.c_size_t()
            if not kernel.ReadProcessMemory(handle, game["base"] + section.VirtualAddress + offset, buffer, size, ctypes.byref(count)) or count.value != size:
                raise ctypes.WinError(ctypes.get_last_error())
            pieces.append(buffer.raw)
        code = b"".join(pieces)
    finally:
        kernel.CloseHandle(handle)
    snapshot.write_bytes(code)
    meta = {"pid": args.pid, "game_path": str(binary), "game_sha256": EXPECTED, "game_base": game["base"],
            "section_rva": section.VirtualAddress, "bytes": len(code), "text_sha256": hashlib.sha256(code).hexdigest(),
            "process_access": "QUERY_INFORMATION | VM_READ", "writes_process_memory": False}
    manifest_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")

pdata = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".pdata")
unwind = pe.get_data(pdata.VirtualAddress, pdata.Misc_VirtualSize)
functions = sorted((start, end) for start, end, _ in struct.iter_unpack("<III", unwind[:len(unwind) // 12 * 12]) if start and end > start)
starts = [s for s, _ in functions]
def owner(rva):
    index = bisect.bisect_right(starts, rva) - 1
    return functions[index] if index >= 0 and rva < functions[index][1] else None

tokens = ["ResolutionScaling", "ContentScale", "ContentScaleBase", "ContentScaleActual", "ContentScalePF",
          "mDynamicResolutionContentScale", "mContentScaleActivePF", "mContentScale2D", "mScreenSize", "ScreenTrim"]
targets = {}
for token in tokens:
    at = 0
    while (at := disk.find(token.encode() + b"\0", at)) >= 0:
        if at == 0 or disk[at - 1] == 0:
            targets[pe.get_rva_from_offset(at)] = token
        at += len(token)

# Validate compact RIP-relative LEA candidates with Capstone before accepting xrefs.
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
references, selected = [], {owner(rva) for rva in args.functions}
for match in re.finditer(rb"[\x48-\x4f]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d].{4}", code, re.DOTALL):
    rva = section.VirtualAddress + match.start()
    target = rva + 7 + struct.unpack_from("<i", match.group(), 3)[0]
    if target not in targets:
        continue
    decoded = list(md.disasm_lite(match.group(), rva))
    if len(decoded) != 1 or decoded[0][1] != 7 or decoded[0][2] != "lea":
        continue
    fn = owner(rva)
    selected.add(fn)
    references.append({"instruction_rva": hex(rva), "target_rva": hex(target), "name": targets[target],
                       "function": [hex(x) for x in fn] if fn else None})

rip = re.compile(r"\[rip ([+-]) (0x[0-9a-f]+|[0-9]+)\]")
for start, end in sorted(s for s in selected if s):
    if start < section.VirtualAddress or end > section.VirtualAddress + len(code) or end - start > 512 * 1024:
        continue
    lines = [f"Live PID {args.pid}; game SHA256 {EXPECTED}", f"RVA function {start:#x} .. {end:#x}"]
    for address, size, mnemonic, operands in md.disasm_lite(code[start - section.VirtualAddress:end - section.VirtualAddress], start):
        note = ""
        match = rip.search(operands)
        if match:
            displacement = int(match[2], 0) * (1 if match[1] == "+" else -1)
            target = address + size + displacement
            note = f" ; data {target:#x}" + (f" {targets[target]}" if target in targets else "")
        lines.append(f"{address:08x} {mnemonic:9} {operands}{note}")
    (args.output / f"function-{start:x}.asm").write_text("\n".join(lines), encoding="utf-8")
report = {"snapshot": meta, "references": references,
          "selected_functions": [[hex(x) for x in fn] for fn in sorted(s for s in selected if s)]}
(args.output / "scaling-xrefs.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps({"references": references, "selected_functions": report["selected_functions"]}, indent=2))
