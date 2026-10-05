"""Validate live API entries and describe the existing RTSS queue hook, using read-only process access."""
import argparse
import configparser
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pid", type=int)
parser.add_argument("ini", type=Path)
parser.add_argument("modules", type=Path)
parser.add_argument("report", type=Path)
args = parser.parse_args()
config = configparser.ConfigParser()
config.optionxform = str
config.read(args.ini, encoding="utf-8-sig")
settings = config["Capture"]
if settings.getint("Pid") != args.pid:
    raise ValueError("Process ID mismatch")
modules = json.loads(args.modules.read_text(encoding="utf-8-sig"))
core = next(m for m in modules if m["name"].lower() == "d3d12core.dll")
if hashlib.sha256(Path(core["path"]).read_bytes()).hexdigest() != settings["CoreSha256"]:
    raise ValueError("Core hash mismatch")
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

report = {"pid": args.pid, "read_only_process_access": True, "methods": []}
try:
    for name in ("Reset", "ResourceBarrier", "ExecuteCommandLists"):
        address = core["base"] + int(settings[name + "Rva"], 0)
        expected = bytes.fromhex(settings[name + "Bytes"])
        actual = read(address, 16)
        row = {"name": name, "address": hex(address), "live_bytes": actual.hex(), "matches_external_helper": actual == expected}
        if actual != expected:
            if name != "ExecuteCommandLists" or actual[0] != 0xE9 or actual[5:] != expected[5:]:
                raise RuntimeError(f"Unexpected API patch: {name}")
            relay = address + 5 + struct.unpack_from("<i", actual, 1)[0]
            code = read(relay, 6)
            if code[:2] != b"\xff\x25":
                raise RuntimeError("Unknown queue relay")
            target = struct.unpack("<Q", read(relay + 6 + struct.unpack_from("<i", code, 2)[0], 8))[0]
            owner = next(m for m in modules if m["base"] <= target < m["base"] + m["size"])
            if owner["name"].lower() != "rtsshooks64.dll":
                raise RuntimeError("Only the inspected RTSS hook can be chained")
            digest = hashlib.sha256(Path(owner["path"]).read_bytes()).hexdigest()
            target_bytes = read(target, 16).hex()
            settings["ExistingQueueHookOwnerSha256"] = digest
            settings["ExistingQueueHookRva"] = hex(target - owner["base"])
            settings["ExistingQueueHookBytes"] = target_bytes
            row["existing_hook"] = {"relay": hex(relay), "target": hex(target), "owner": owner, "owner_sha256": digest, "target_bytes": target_bytes}
        report["methods"].append(row)
    with args.ini.open("w", encoding="ascii", newline="\n") as output:
        config.write(output, space_around_delimiters=False)
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
finally:
    kernel.CloseHandle(handle)
