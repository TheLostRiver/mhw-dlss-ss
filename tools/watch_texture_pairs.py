"""Request one additional Low snapshot after its first completed save; do not change any game settings."""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pid", type=int)
parser.add_argument("directory", type=Path)
args = parser.parse_args()
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
kernel.OpenEventW.restype = wintypes.HANDLE
kernel.SetEvent.argtypes = [wintypes.HANDLE]
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
seen = set(args.directory.glob("*/metadata.json"))
requested = False
low = []
deadline = time.monotonic() + 1800
print("Waiting for automatic Low capture; will request one additional snapshot for temporal ROI comparison.", flush=True)
while time.monotonic() < deadline:
    for metadata in sorted(args.directory.glob("*/metadata.json")):
        if metadata in seen:
            continue
        try:
            capture = json.loads(metadata.read_text())
        except (ValueError, OSError):
            continue
        seen.add(metadata)
        setting = capture["ResolutionScaling"]
        print(json.dumps({"saved": str(metadata), "setting": setting}), flush=True)
        if setting == "Low":
            low.append(metadata)
            if not requested:
                event = kernel.OpenEventW(0x0002, False, f"Local\\MhwTextureProbe.{args.pid}.Capture")
                if not event:
                    raise ctypes.WinError(ctypes.get_last_error())
                try:
                    if not kernel.SetEvent(event):
                        raise ctypes.WinError(ctypes.get_last_error())
                finally:
                    kernel.CloseHandle(event)
                requested = True
                print("Additional Low snapshot requested; the game keeps rendering normally.", flush=True)
        elif setting == "High" and low:
            print(json.dumps({"complete": True, "low_snapshots": len(low), "restored_high": str(metadata)}), flush=True)
            raise SystemExit(0)
    time.sleep(0.5)
print("Watcher timeout; no game settings or observer shutdown were changed.", flush=True)
