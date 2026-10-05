"""Summarize candidate resource uses without treating descriptor-table proximity as proof of a shader read."""
import argparse
from collections import Counter
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("capture", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
rows = [json.loads(line) for line in args.capture.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
setting = "unknown"
groups = Counter()
frame_settings = {}
shader_files = []
statuses = []
for row in rows:
    if row["event"] == "setting_marker":
        setting = row.get("setting", "unknown")
    elif row["event"] == "upscale_return":
        frame_settings[row["frame"]] = setting
    elif row["event"] == "candidate_pass":
        groups[(frame_settings.get(row["frame"], setting), row["kind"], row["pso"], tuple(row["viewport"]), row["shader_available"])] += 1
    elif row["event"] == "shader_saved":
        shader_files.append(row)
    elif row["event"] == "status":
        statuses.append(row)
summary = {
    "capture": str(args.capture), "events": dict(Counter(row["event"] for row in rows)),
    "frames_by_setting": dict(Counter(frame_settings.values())),
    "candidates": [{"setting": key[0], "kind": key[1], "pso": key[2], "viewport": key[3], "shader_available": key[4], "records": value}
                   for key, value in groups.most_common()],
    "shader_files": shader_files, "last_status": statuses[-1] if statuses else None,
    "maximum_reported_drops": max((row.get("dropped", 0) for row in statuses), default=0),
    "all_rows_are_candidates_not_confirmed_readers": True,
    "limitations": ["Root signatures for the game passes may predate the observer.",
                    "Scanning 32 slots can find resources beyond the table range actually used by a shader.",
                    "v3 does not follow compute UAV outputs; its compute render_targets field is previous graphics state.",
                    "Cache saturation means the binding graph is incomplete."]
}
args.output.write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(json.dumps({key: summary[key] for key in ("events", "frames_by_setting", "maximum_reported_drops")}, indent=2))
