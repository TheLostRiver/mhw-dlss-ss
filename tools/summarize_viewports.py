"""Summarize sampled raster windows; configuration time is not GPU application time."""
import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("capture", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if args.capture.resolve() == args.output.resolve():
    parser.error("Output cannot overwrite the original capture.")
data = args.capture.read_text(encoding="utf-8-sig")
lines = data.splitlines()
records, incomplete_last_line = [], False
for index, line in enumerate(lines):
    if not line.strip():
        continue
    try:
        records.append(json.loads(line))
    except json.JSONDecodeError:
        if index == len(lines) - 1 and not data.endswith('\n'):
            incomplete_last_line = True
        else:
            raise
starts = [index for index, row in enumerate(records) if row.get("event") == "start" and row.get("version") == 2]
if starts:
    records = records[starts[-1]:]
markers = sorted((row for row in records if row.get("event") == "setting_marker"), key=lambda row: row["tick_ms"])
times = [row["tick_ms"] for row in markers]
counts, geometry = Counter(), Counter()
dispatch = Counter()
windows = Counter()
rejected = Counter()
dropped = 0
for window in records:
    if window.get("event") != "raster_window":
        continue
    begin, end = window["begin_tick_ms"], window["end_tick_ms"]
    marker_index = bisect_right(times, begin) - 1
    dropped = max(dropped, window.get("dropped_total", 0))
    if marker_index < 0:
        rejected["before_first_marker"] += 1
        continue
    if begin < times[marker_index] + 2000:
        rejected["near_setting_transition"] += 1
        continue
    if marker_index + 1 < len(times) and end > times[marker_index + 1] - 2000:
        rejected["near_setting_transition"] += 1
        continue
    setting = markers[marker_index]["ResolutionScaling"]
    windows[setting] += 1
    for group in window["groups"]:
        if group["kind"] == "dispatch":
            dispatch[(setting, tuple(group["thread_groups"]))] += group["samples"]
            continue
        viewport = group["first_viewport"]
        scissor = group["first_scissor"]
        if group["viewport_count"] < 1 or any(value is None for value in viewport):
            rejected["sample_without_known_viewport"] += group["samples"]
            continue
        key = (setting, group["kind"], tuple(viewport[:4]), tuple(scissor), group["geometry_bucket"])
        counts[key] += group["samples"]
        x, y, width, height = viewport[:4]
        # These are screen-shaped geometry candidates, not proven main-camera passes.
        if (group["kind"] == "indexed" and group["geometry_bucket"] >= 1 and
            group["viewport_count"] == 1 and group["scissor_count"] == 1 and
            width >= 640 and height >= 360 and abs(width / height - 16 / 9) < .02 and
            abs(x) < .01 and abs(y) < .01 and scissor == [0, 0, int(width), int(height)]):
            geometry[(setting, width, height)] += group["samples"]
report = {
    "capture": str(args.capture.resolve()), "incomplete_last_line_skipped": incomplete_last_line,
    "status_events": [row for row in records if row.get("event") != "raster_window"],
    "accepted_windows_by_observed_setting": dict(windows), "rejected": dict(rejected),
    "observer_dropped_records": dropped,
    "screen_shaped_geometry_candidates": [
        {"observed_setting": key[0], "viewport": [key[1], key[2]], "samples": count}
        for key, count in geometry.most_common()],
    "raster_groups": [
        {"observed_setting": key[0], "kind": key[1], "viewport_xywh": key[2],
         "scissor": key[3], "geometry_bucket": key[4], "samples": count}
        for key, count in counts.most_common()],
    "dispatch_groups": [{"observed_setting": key[0], "thread_groups": key[1], "samples": count}
                        for key, count in dispatch.most_common()],
    "limitations": [
        "Direct Draw/DrawIndexed/Dispatch calls using the inspected method implementations are sampled, not exhaustively recorded.",
        "ExecuteIndirect, shader-side sample coverage and other method implementations are not covered.",
        "Only the first viewport/scissor in an array is recorded; counts are retained.",
        "Screen-shaped indexed draws are candidates, not proven main-camera passes without render-target/shader identification.",
        "Two-second transition margins reduce ambiguity but do not prove when GPU settings changed.",
        "Dispatch group counts do not give pixel resolution without knowing shader thread-group dimensions.",
        "No GPU pixels, GPU timings or performance gains are measured."
    ]
}
args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps({key: report[key] for key in ("accepted_windows_by_observed_setting", "observer_dropped_records", "screen_shaped_geometry_candidates", "rejected")}, ensure_ascii=False, indent=2))
