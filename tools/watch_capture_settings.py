"""Observe selected graphics settings on disk and timestamp changes; never modify them."""
import argparse
import ctypes
from datetime import datetime
import json
from pathlib import Path
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("settings", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--seconds", type=float, default=300)
args = parser.parse_args()
if args.settings.resolve() == args.output.resolve():
    parser.error("Output cannot overwrite the settings file.")
if not 1 <= args.seconds <= 3600:
    parser.error("Observation duration must be between 1 and 3600 seconds.")
get_tick = ctypes.WinDLL("kernel32", use_last_error=True).GetTickCount64
get_tick.argtypes = []
get_tick.restype = ctypes.c_ulonglong
keys = {"Resolution", "ResolutionScaling", "Anti-Aliasing", "NVIDIA DLSS", "DirectX12Enable"}
args.output.parent.mkdir(parents=True, exist_ok=True)
previous = None
deadline = time.monotonic() + args.seconds
with args.output.open("a", encoding="utf-8", buffering=1) as output:
    while time.monotonic() < deadline:
        try:
            values = {}
            for line in args.settings.read_text(encoding="utf-8-sig").splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    if key.strip() in keys:
                        values[key.strip()] = value.strip()
            # Ignore an incomplete file during the game's config write.
            if keys.issubset(values) and values != previous:
                event = {"observed_at": datetime.now().astimezone().isoformat(),
                         "tick_ms": get_tick(), "settings_on_disk": values,
                         "meaning": "Configuration file observation, not proof of GPU frame application."}
                text = json.dumps(event, ensure_ascii=False)
                output.write(text + "\n")
                print(text, flush=True)
                previous = values
        except (OSError, UnicodeError):
            pass
        time.sleep(.5)
print("Settings observation finished.", flush=True)
