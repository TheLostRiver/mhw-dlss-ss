"""Read a known command-list vtable in the current game; never write process memory."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pid", type=int)
parser.add_argument("capture", type=Path)
parser.add_argument("modules", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
if args.output.resolve() in (args.capture.resolve(), args.modules.resolve()):
    parser.error("Output must not overwrite input evidence.")
records = [json.loads(line) for line in args.capture.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
stage = next(row for row in reversed(records) if row.get("event") == "upstream_stage")
modules = json.loads(args.modules.read_text(encoding="utf-8-sig"))
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel.ReadProcessMemory.restype = wintypes.BOOL
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.CloseHandle.restype = wintypes.BOOL
handle = kernel.OpenProcess(0x0410, False, args.pid)
if not handle:
    raise ctypes.WinError(ctypes.get_last_error())
def read(address, size):
    buffer = ctypes.create_string_buffer(size)
    actual = ctypes.c_size_t()
    if not kernel.ReadProcessMemory(handle, address, buffer, size, ctypes.byref(actual)) or actual.value != size:
        raise ctypes.WinError(ctypes.get_last_error())
    return buffer.raw
try:
    command_list = int(stage["command_list"], 16)
    vtable = struct.unpack("<Q", read(command_list, 8))[0]
    functions = struct.unpack("<26Q", read(vtable, 26 * 8))
    if functions[21] != int(stage["viewport_method_address"], 16):
        raise RuntimeError("The live vtable no longer matches the recorded command list; stop and recapture.")
    indices = {"Reset": 10, "ClearState": 11, "DrawInstanced": 12, "DrawIndexedInstanced": 13,
               "Dispatch": 14, "RSSetViewports": 21, "RSSetScissorRects": 22, "SetPipelineState": 25}
    methods = []
    for name, index in indices.items():
        address = functions[index]
        module = next((m for m in modules if m["base"] <= address < m["base"] + m["size"]), None)
        if not module or module["name"].lower() != "d3d12core.dll":
            raise RuntimeError(f"Unexpected implementation module for {name}: {module}")
        methods.append({"name": name, "index": index, "address": hex(address), "rva": hex(address - module["base"]),
                        "first_16_bytes": read(address, 16).hex()})
    core = next(m for m in modules if m["name"].lower() == "d3d12core.dll")
    report = {"pid": args.pid, "command_list": hex(command_list), "vtable": hex(vtable), "module": core,
              "module_sha256": hashlib.sha256(Path(core["path"]).read_bytes()).hexdigest(), "methods": methods,
              "method": "ReadProcessMemory only; no hooks or process writes performed."}
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
finally:
    kernel.CloseHandle(handle)
