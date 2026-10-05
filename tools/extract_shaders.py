"""Extract embedded DXBC containers and disassemble with the Windows shader compiler.

The input DLL is treated solely as a byte stream and is never loaded.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import struct

import pefile


def disassemble(data: bytes) -> tuple[int, str | None]:
    compiler = ctypes.WinDLL(str(Path(os.environ["SystemRoot"]) / "System32" / "d3dcompiler_47.dll"))
    function = compiler.D3DDisassemble
    function.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wintypes.UINT, ctypes.c_char_p,
                         ctypes.POINTER(ctypes.c_void_p)]
    function.restype = ctypes.c_long
    memory = ctypes.create_string_buffer(data)
    blob = ctypes.c_void_p()
    result = function(memory, len(data), 0, None, ctypes.byref(blob))
    if not blob:
        return result & 0xFFFFFFFF, None
    table = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    release = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(table[2])
    get_pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(table[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(table[4])
    try:
        text = ctypes.string_at(get_pointer(blob), get_size(blob)).decode("utf-8", "replace").rstrip("\0")
        return result & 0xFFFFFFFF, text
    finally:
        release(blob)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    pe = pefile.PE(data=data, fast_load=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    position = 0
    while True:
        position = data.find(b"DXBC", position)
        if position < 0:
            break
        start = position
        position += 4
        if start + 32 > len(data):
            continue
        version, size, count = struct.unpack_from("<III", data, start + 20)
        if version != 1 or not 32 <= size <= len(data) - start or not 1 <= count <= 64:
            continue
        if 32 + count * 4 > size:
            continue
        offsets = struct.unpack_from(f"<{count}I", data, start + 32)
        chunks = []
        valid = True
        for offset in offsets:
            if offset < 32 + count * 4 or offset + 8 > size:
                valid = False
                break
            length = struct.unpack_from("<I", data, start + offset + 4)[0]
            if offset + 8 + length > size:
                valid = False
                break
            chunks.append(data[start + offset:start + offset + 4].decode("ascii", "replace"))
        if not valid:
            continue
        shader = data[start:start + size]
        rva = pe.get_rva_from_offset(start)
        name = f"shader_{rva:08x}"
        (args.output_dir / f"{name}.dxbc").write_bytes(shader)
        result, text = disassemble(shader)
        if text is not None:
            (args.output_dir / f"{name}.asm").write_text(text, encoding="utf-8")
        entries.append({"name": name, "offset": hex(start), "rva": hex(rva), "size": size,
                        "sha256": hashlib.sha256(shader).hexdigest(), "chunks": chunks,
                        "disassemble_hresult": hex(result), "has_disassembly": text is not None and result == 0})
        position = start + size
    report = {"binary_sha256": hashlib.sha256(data).hexdigest(), "shaders": entries}
    (args.output_dir / "manifest.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    pe.close()


if __name__ == "__main__":
    main()
