"""Static x64 RIP-relative string references and PE unwind function disassembly."""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / ".packages"))
import capstone
import pefile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--pattern", default=r"InlGetJitter|Proj jitter|SubmitPostTAACommands|GetJitter|Creating DLSS feature|DLSSGetRenderScale")
    parser.add_argument("--functions", nargs="*", type=lambda value: int(value, 0), default=[])
    parser.add_argument("--targets", nargs="*", type=lambda value: int(value, 0), default=[])
    parser.add_argument("--imports", nargs="*", default=[], help="Also trace RIP-relative calls/references to these import names.")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for destination in (args.output, args.output.with_suffix(".json")):
        if destination.resolve() in (args.binary.resolve(), args.evidence.resolve()):
            parser.error("Outputs must not overwrite input files.")
    evidence = json.loads(args.evidence.read_text(encoding="utf-8"))
    data = args.binary.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != evidence["sha256"]:
        parser.error("The binary does not match the static evidence SHA-256.")
    pe = pefile.PE(data=data)
    base = pe.OPTIONAL_HEADER.ImageBase
    functions = sorted((item.struct.BeginAddress, item.struct.EndAddress)
                       for item in pe.DIRECTORY_ENTRY_EXCEPTION)
    starts = [item[0] for item in functions]
    def enclosing(rva):
        index = bisect.bisect_right(starts, rva) - 1
        if index >= 0 and rva < functions[index][1]:
            return functions[index]
        return None
    strings = {int(item["rva"], 16): item["text"] for item in evidence["strings"] if item["rva"]}
    wanted = {rva: value for rva, value in strings.items() if re.search(args.pattern, value)}
    for rva in args.targets:
        wanted[rva] = strings.get(rva, f"data target {rva:#x}")
        strings.setdefault(rva, wanted[rva])
    requested_imports = set(args.imports)
    found_imports = set()
    for library in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for item in library.imports:
            name = item.name.decode("ascii", "replace") if item.name else f"ordinal:{item.ordinal}"
            if name in requested_imports:
                rva = item.address - base
                wanted[rva] = f"IAT {library.dll.decode('ascii', 'replace')}!{name}"
                strings[rva] = wanted[rva]
                found_imports.add(name)
    if requested_imports - found_imports:
        parser.error("Imports not found: " + ", ".join(sorted(requested_imports - found_imports)))
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    disassembler.skipdata = True
    rip = re.compile(r"\[rip ([+-]) (0x[0-9a-f]+|[0-9]+)\]")
    def target_of(address, size, operands):
        match = rip.search(operands)
        if not match:
            return None
        distance = int(match.group(2), 0)
        return address + size + (distance if match.group(1) == "+" else -distance)
    references = []
    selected = {enclosing(rva) for rva in args.functions}
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        for address, size, mnemonic, operands in disassembler.disasm_lite(section.get_data(), section.VirtualAddress):
            target = target_of(address, size, operands)
            if target not in wanted:
                continue
            owner = enclosing(address)
            selected.add(owner)
            references.append({"instruction_rva": hex(address), "target_rva": hex(target),
                               "function_rva": hex(owner[0]) if owner else None,
                               "string": wanted[target]})
    lines = [f"Binary: {args.binary}", f"SHA256: {digest}", f"Preferred image base: {base:#x}",
             "All instruction addresses below are RVAs. Static candidates, not runtime-validated hooks.", ""]
    for start, end in sorted(item for item in selected if item):
        lines.append(f"FUNCTION {start:#x} .. {end:#x}")
        body = pe.get_data(start, end - start)
        for address, size, mnemonic, operands in disassembler.disasm_lite(body, start):
            target = target_of(address, size, operands)
            note = f" ; {strings[target]}" if target in strings else ""
            lines.append(f"{address:08x}  {mnemonic:8} {operands}{note}")
        lines.append("")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    summary = {"binary_sha256": digest, "references": references,
               "functions": [{"start": hex(start), "end": hex(end)} for start, end in sorted(item for item in selected if item)]}
    args.output.with_suffix(".json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    pe.close()


if __name__ == "__main__":
    main()
