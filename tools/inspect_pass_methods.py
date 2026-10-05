"""Inspect existing API detours with ReadProcessMemory only; never writes to the game."""
import argparse
import configparser
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pid", type=int)
parser.add_argument("ini", type=Path)
parser.add_argument("modules", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
modules = json.loads(args.modules.read_text(encoding="utf-8-sig"))
core = next(module for module in modules if module["name"].lower() == "d3d12core.dll")
config = configparser.ConfigParser()
config.optionxform = str
ini_bytes = args.ini.read_bytes()
config.read_string(ini_bytes.decode("utf-16" if ini_bytes.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"))
section = config["Capture"]
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel.ReadProcessMemory.restype = wintypes.BOOL
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
handle = kernel.OpenProcess(0x0410, False, args.pid)
if not handle:
    raise ctypes.WinError(ctypes.get_last_error())

def read(address, size):
    buffer = ctypes.create_string_buffer(size)
    actual = ctypes.c_size_t()
    if not kernel.ReadProcessMemory(handle, address, buffer, size, ctypes.byref(actual)) or actual.value != size:
        raise ctypes.WinError(ctypes.get_last_error())
    return buffer.raw

report = []
try:
    for key in list(section):
        if not key.endswith("Rva") or key.startswith("Existing"):
            continue
        name = key[:-3]
        address = core["base"] + int(section[key], 0)
        actual = read(address, 16)
        if actual.hex() == section[name + "Bytes"]:
            continue
        destination, hops = address, []
        for _ in range(4):
            code = read(destination, 16)
            if code[0] == 0xE9:
                destination += 5 + struct.unpack_from("<i", code, 1)[0]
            elif code[:2] == b"\xff\x25":
                destination = struct.unpack("<Q", read(destination + 6 + struct.unpack_from("<i", code, 2)[0], 8))[0]
            else:
                break
            hops.append(hex(destination))
        owner = next((module for module in modules if module["base"] <= destination < module["base"] + module["size"]), None)
        report.append({"name": name, "actual_bytes": actual.hex(), "hops": hops, "owner": owner,
                       "target_rva": hex(destination - owner["base"]) if owner else None,
                       "target_bytes": read(destination, 16).hex()})
        print(name, "->", owner["name"] if owner else "unknown", report[-1]["target_rva"])
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
finally:
    kernel.CloseHandle(handle)
