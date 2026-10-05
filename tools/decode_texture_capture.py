"""Decode captured GPU buffers to diagnostic PNGs and numeric region summaries; never touches the game."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

def packed_unsigned_float(bits, mantissa_bits):
    exponent = (bits >> mantissa_bits).astype(np.int32)
    mantissa = (bits & ((1 << mantissa_bits) - 1)).astype(np.float32)
    result = np.ldexp(1.0 + mantissa / (1 << mantissa_bits), exponent - 15)
    result = np.where(exponent == 0, np.ldexp(mantissa, 1 - 15 - mantissa_bits), result)
    return np.where(exponent == 31, np.where(mantissa == 0, np.inf, np.nan), result)

def load_texture(directory, entry):
    data = (directory / entry["file"]).read_bytes()
    if len(data) != entry["total_bytes"]:
        raise ValueError(f"Incomplete buffer: {entry['file']}")
    height, width = entry["height"], entry["width"]
    if entry["row_bytes"] != width * 4 or entry["rows"] != height:
        raise ValueError("Unsupported footprint")
    raw = np.ndarray((height, width), dtype="<u4", buffer=data, offset=entry["offset"], strides=(entry["row_pitch"], 4)).copy()
    if entry["format"] == 26:  # R11G11B10_FLOAT, unsigned E5M6/E5M6/E5M5.
        array = np.stack([packed_unsigned_float(raw & 2047, 6), packed_unsigned_float((raw >> 11) & 2047, 6),
                          packed_unsigned_float((raw >> 22) & 1023, 5)], axis=-1)
    elif entry["format"] == 41:
        array = raw.view("<f4")[..., None]
    elif entry["format"] == 34:
        array = raw.view("<f2").reshape(height, width, 2).astype(np.float32)
    else:
        raise ValueError(f"Unsupported format: {entry['format']}")
    return raw, array

def statistics(raw, array):
    finite = np.isfinite(array)
    channels = []
    for index in range(array.shape[-1]):
        values = array[..., index][finite[..., index]]
        channels.append({"min": float(values.min()) if values.size else None,
                         "p01_p50_p99": np.percentile(values, [1, 50, 99]).tolist() if values.size else [],
                         "max": float(values.max()) if values.size else None})
    return {"pixels": int(raw.size), "raw_zero_fraction": float(np.mean(raw == 0)),
            "finite_fraction": float(finite.mean()), "channels": channels}

def preview(array, name):
    clean = np.nan_to_num(array, nan=0.0, posinf=0.0, neginf=0.0)
    if name in ("Color", "Output"):
        # A fixed visualization transform, not a claim about the game's display transfer function.
        clean = np.maximum(clean, 0)
        result = np.power(clean / (1 + clean), 1 / 2.2)
    elif name == "Depth":
        # Inverted-Z depth is highly concentrated near zero; percentile view exposes structure.
        positive = clean[clean > 0]
        high = float(np.percentile(positive, 99.5)) if positive.size else 1.0
        result = np.repeat(np.clip(clean / max(high, 1e-9), 0, 1), 3, axis=-1)
    else:
        # Direction/magnitude view of the raw intermediate vectors; NGX scale is in metadata.
        bound = max(float(np.percentile(np.abs(clean), 99)), 1)
        result = np.empty((*clean.shape[:2], 3), dtype=np.float32)
        result[..., :2] = np.clip(0.5 + clean / (2 * bound), 0, 1)
        result[..., 2] = np.clip(np.linalg.norm(clean, axis=-1) / bound, 0, 1)
    return Image.fromarray((np.clip(result, 0, 1) * 255).astype(np.uint8))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory
    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
    report = {"metadata": metadata, "preview_is_diagnostic_transform": True, "textures": []}
    tiles = []
    for entry in metadata["textures"]:
        raw, array = load_texture(directory, entry)
        height, width = raw.shape
        x, y = width * 3 // 4, height * 3 // 4
        row = {"name": entry["name"], "all": statistics(raw, array), "regions_at_75_percent": {}}
        for region, region_slice in (("top_left", (slice(0, y), slice(0, x))), ("right", (slice(0, y), slice(x, width))),
                                     ("bottom", (slice(y, height), slice(0, width)))):
            row["regions_at_75_percent"][region] = statistics(raw[region_slice], array[region_slice])
        # Boundaries of raw nonzero content are evidence, not an automatic declaration of the valid region.
        nonzero = raw != 0
        ys, xs = np.nonzero(nonzero)
        row["nonzero_bbox"] = [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1] if xs.size else None
        report["textures"].append(row)
        picture = preview(array, entry["name"])
        picture.save(directory / (entry["name"] + "-preview.png"))
        picture.thumbnail((768, 432))
        tile = Image.new("RGB", (768, 462), "#202328")
        tile.paste(picture, (0, 30))
        draw = ImageDraw.Draw(tile)
        draw.text((10, 8), f"{entry['name']} | {width}x{height} | {metadata['ResolutionScaling']}", fill="white")
        # Yellow lines show the 75% candidate boundaries solely for visual comparison.
        draw.line((576, 30, 576, 461), fill="#ffdb44", width=1)
        draw.line((0, 354, 767, 354), fill="#ffdb44", width=1)
        tiles.append(tile)
    sheet = Image.new("RGB", (1536, 924), "#202328")
    for index, tile in enumerate(tiles):
        sheet.paste(tile, ((index % 2) * 768, (index // 2) * 462))
    sheet.save(directory / "contact-sheet.png")
    (directory / "pixel-summary.json").write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
    print(json.dumps({"setting": metadata["ResolutionScaling"], "result": metadata["evaluate_result"],
                      "textures": [{"name": row["name"], "nonzero_bbox": row["nonzero_bbox"], "raw_zero_fraction": row["all"]["raw_zero_fraction"]} for row in report["textures"]]}, indent=2))

if __name__ == "__main__":
    main()
