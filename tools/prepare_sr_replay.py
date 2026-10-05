"""Prepare a standalone, history-reset DLSS SR replay of the captured valid scene rectangle."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("capture", type=Path)
parser.add_argument("runtime_directory", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
source = args.capture.resolve()
metadata = json.loads((source / "metadata.json").read_text())
if metadata["ResolutionScaling"] != "Low" or not metadata["gpu_fence_completed"]:
    raise ValueError("A GPU-fence-completed Low capture is required")
by_name = {entry["name"]: entry for entry in metadata["textures"]}
width, height = by_name["Color"]["width"], by_name["Color"]["height"]
render_width, render_height = width * 3 // 4, height * 3 // 4
for name, fmt in (("Color", 26), ("Depth", 41), ("MotionVectors", 34)):
    entry = by_name[name]
    if (entry["width"], entry["height"], entry["format"], entry["offset"], entry["row_pitch"]) != (width, height, fmt, 0, width * 4):
        raise ValueError("Capture footprint is not supported by this replay")
    if (source / entry["file"]).stat().st_size != entry["total_bytes"]:
        raise ValueError("Incomplete input")
runtime = (args.runtime_directory / "nvngx_dlss.dll").resolve()
digest = hashlib.sha256(runtime.read_bytes()).hexdigest()
params = metadata["parameters"]
settings = {
    "InputDirectory": str(source), "RuntimeDirectory": str(runtime.parent), "ExpectedRuntimeSha256": digest,
    "SourceWidth": width, "SourceHeight": height, "RenderWidth": render_width, "RenderHeight": render_height,
    "OutputWidth": width, "OutputHeight": height, "Quality": 2, "Preset": 11,
    # MHWSS decodes vectors with allocation dimensions. Convert to render-pixel units.
    # This is a derived conversion; one reset frame cannot validate temporal motion correctness.
    "JitterX": params["Jitter.Offset.X"] * render_width / width,
    "JitterY": params["Jitter.Offset.Y"] * render_height / height,
    "MVScaleX": params["MV.Scale.X"] * render_width / width,
    "MVScaleY": params["MV.Scale.Y"] * render_height / height,
}
config = configparser.ConfigParser()
config.optionxform = str
config["Replay"] = {key: str(value) for key, value in settings.items()}
args.output.mkdir(parents=True, exist_ok=True)
with (args.output / "replay.ini").open("w", encoding="utf-16") as output:
    config.write(output, space_around_delimiters=False)
manifest = {"capture": str(source), "input_from_real_low_geometry": True, "crop_only_no_resampling": True,
            "history_reset": True, "single_frame_only": True, "motion_and_jitter_scale_inferred_not_temporally_validated": True,
            "settings": settings, "source_metadata": metadata}
(args.output / "input-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
print(json.dumps(settings, indent=2))
