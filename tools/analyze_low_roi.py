"""Quantify the 75% scene rectangle from saved input pixels; no runtime/game access."""
import argparse
import json
from pathlib import Path
import numpy as np
from decode_texture_capture import load_texture

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("low", type=Path)
parser.add_argument("high", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
report = {"method": "raw pixel analysis of GPU-fence-completed captures", "captures": []}
for directory in (args.low, args.high):
    metadata = json.loads((directory / "metadata.json").read_text())
    capture = {"directory": str(directory), "setting": metadata["ResolutionScaling"], "textures": []}
    for entry in metadata["textures"]:
        raw, array = load_texture(directory, entry)
        height, width = raw.shape
        x, y = width * 3 // 4, height * 3 // 4
        outside = np.concatenate((raw[:y, x:].ravel(), raw[y:, :].ravel()))
        values, counts = np.unique(outside, return_counts=True)
        most_common = int(np.argmax(counts))
        raw_mode = values[most_common]
        yy, xx = np.nonzero(raw != raw_mode)
        row = {"name": entry["name"], "candidate_roi": [0, 0, x, y], "outside_pixels": int(outside.size),
               "outside_unique_raw_values": int(values.size), "outside_mode_raw": hex(int(raw_mode)),
               "outside_mode_fraction": float(counts[most_common] / outside.size),
               "non_mode_bbox": [int(xx.min()), int(yy.min()), int(xx.max()) + 1, int(yy.max()) + 1] if xx.size else None}
        if entry["format"] == 34:
            row["outside_mode_rg16f"] = np.array([raw_mode], dtype="<u4").view("<f2").astype(float).tolist()
        capture["textures"].append(row)
    report["captures"].append(capture)
report["conclusion"] = {
    "low_motion_active_difference_bbox": report["captures"][0]["textures"][2]["non_mode_bbox"],
    "geometry_cross_check": "Independent indexed-draw capture recorded matching 1920x1080 viewports and scissors in Low.",
    "color_and_depth": "Visual scene structure agrees inside that rectangle; exterior includes discontinuous scene content, not zeros.",
    "not_proven": ["The exact downstream resampling shader and constants", "Motion-vector temporal sign/units after SR conversion", "FPS gains"],
    "user_observation": "User reports the final Low image was complete, without obvious stitching or cropping. This supports a downstream crop/scale step; it is not a GPU trace of that step."
}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps(report["conclusion"], indent=2))
