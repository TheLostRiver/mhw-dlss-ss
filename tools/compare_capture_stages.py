"""Compare stage metadata against observed configuration-file change times."""
import argparse
from bisect import bisect_right
from collections import Counter
import json
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("capture", type=Path)
parser.add_argument("markers", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if args.output.resolve() in (args.capture.resolve(), args.markers.resolve()):
    parser.error("The report must not overwrite an input file.")
records = [json.loads(line) for line in args.capture.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
markers = sorted((json.loads(line) for line in args.markers.read_text(encoding="utf-8-sig").splitlines() if line.strip()), key=lambda row: row["tick_ms"])
times = [row["tick_ms"] for row in markers]
groups = Counter()
evaluations = {row["call"]: row for row in records if row.get("event") == "evaluate"}
jitter_pairs = 0
jitter_matches = 0
for row in records:
    if row.get("event") not in ("evaluate", "upstream_stage"):
        continue
    index = bisect_right(times, row["tick_ms"]) - 1
    setting = markers[index]["settings_on_disk"].get("ResolutionScaling") if index >= 0 else "unobserved"
    if row["event"] == "evaluate":
        signature = {"observed_setting": setting, "stage": "NGX_evaluate", "input": [row.get("active_width"), row.get("active_height")], "output": [row.get("output_width"), row.get("output_height")], "quality": row.get("quality"), "result": row.get("result")}
    else:
        source = row.get("source") or {}
        signature = {"observed_setting": setting, "stage": row.get("stage"), "source_size": [source.get("width"), source.get("height")], "source_format": source.get("format"), "context_size_candidate": row.get("context_size_candidate")}
        if row.get("stage") == "upscale_entry_resource":
            evaluated = evaluations.get(row["completed_evaluates"] + 1)
            if evaluated and row.get("auxiliary1"):
                raw_x, raw_y = struct.unpack("<ff", struct.pack("<Q", int(row["auxiliary1"], 16)))
                if isinstance(evaluated.get("jitter_x"), (int, float)) and isinstance(evaluated.get("jitter_y"), (int, float)):
                    jitter_pairs += 1
                    jitter_matches += abs(evaluated["jitter_x"] + .5 * raw_x) < 1e-6 and abs(evaluated["jitter_y"] - .5 * raw_y) < 1e-6
    groups[json.dumps(signature, sort_keys=True)] += 1
report = {
    "capture": str(args.capture.resolve()), "markers": markers,
    "observations": [{**json.loads(key), "samples": count} for key, count in groups.items()],
    "paired_jitter_observations": jitter_pairs,
    "paired_jitter_matches_static_scaling": jitter_matches,
    "limitations": [
        "Settings tags are observations of the configuration file, not proof of GPU application at the same instant.",
        "Resource allocation dimensions do not establish the actual viewport or active content region.",
        "A matched jitter pair checks this call chain's scaling, not the correctness of camera projection or frame history.",
        "No GPU pixels or GPU timings are captured."
    ]
}
args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(report, ensure_ascii=False, indent=2))
