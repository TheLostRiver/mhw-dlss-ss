"""Read PE metadata and relevant strings without loading or executing the binary."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

import pefile


DEFAULT_PATTERN = (
    r"NVSDK|NVNGX|nvngx|MHWSS|InlGetJitter|Jitter|MotionVector|MotionScale|"
    r"RenderScale|RenderWidth|RenderHeight|RenderSize|OutputSize|InputSize|"
    r"PerfQuality|Upscale|Upscaler|DLSS|DLAA|TAA|Velocity|GBuffer|"
    r"Projection|DepthBuffer|RenderResolution|ResolutionScale|\.toml$"
)


def inspect(path: Path, pattern: str) -> dict:
    data = path.read_bytes()
    pe = pefile.PE(data=data, fast_load=True)
    pe.parse_data_directories(directories=[0, 1, 3])
    wanted = re.compile(pattern, re.IGNORECASE)
    strings = []
    for encoding, expression in (
        ("ascii", rb"[\x20-\x7e]{6,}"),
        ("utf-16-le", rb"(?:[\x20-\x7e]\x00){6,}"),
    ):
        for match in re.finditer(expression, data):
            value = match.group().decode(encoding)
            if len(value) > 240 or not wanted.search(value):
                continue
            if value.startswith(("io.", "Set io.", "struct toml::", "class toml::")):
                continue
            try:
                rva = pe.get_rva_from_offset(match.start())
            except pefile.PEFormatError:
                rva = None
            strings.append({"offset": hex(match.start()), "rva": hex(rva) if rva is not None else None,
                            "encoding": encoding, "text": value})
    result = {
        "path": str(path.resolve()), "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data), "image_base": hex(pe.OPTIONAL_HEADER.ImageBase),
        "entrypoint_rva": hex(pe.OPTIONAL_HEADER.AddressOfEntryPoint),
        "entrypoint_section": next((s.Name.rstrip(b"\0").decode("ascii", "replace") for s in pe.sections
                                    if s.contains_rva(pe.OPTIONAL_HEADER.AddressOfEntryPoint)), None),
        "overlay_bytes": len(pe.get_overlay() or b""),
        "sections": [{"name": s.Name.rstrip(b"\0").decode("ascii", "replace"),
                      "rva": hex(s.VirtualAddress), "virtual_size": s.Misc_VirtualSize,
                      "raw_offset": hex(s.PointerToRawData), "raw_size": s.SizeOfRawData,
                      "entropy": round(s.get_entropy(), 4),
                      "executable": bool(s.Characteristics & 0x20000000),
                      "writable": bool(s.Characteristics & 0x80000000)}
                     for s in pe.sections],
        "imports": [{"dll": entry.dll.decode("ascii", "replace"),
                     "names": [item.name.decode("ascii", "replace") if item.name else f"ordinal:{item.ordinal}"
                               for item in entry.imports]}
                    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])],
        "exports": [{"name": item.name.decode("ascii", "replace") if item.name else None,
                     "ordinal": item.ordinal, "rva": hex(item.address)}
                    for item in getattr(getattr(pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", [])],
        "strings": sorted(strings, key=lambda item: int(item["offset"], 16)),
        "runtime_function_count": len(getattr(pe, "DIRECTORY_ENTRY_EXCEPTION", [])),
        "method": "Static file inspection only; the target is not loaded or executed.",
    }
    pe.close()
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--pattern", default=DEFAULT_PATTERN)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = inspect(args.binary, args.pattern)
    if args.output:
        if args.output.resolve() == args.binary.resolve():
            parser.error("The output must not overwrite the inspected binary.")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"Saved static evidence: {args.output}")
        print(f"SHA-256: {result['sha256']}")
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
