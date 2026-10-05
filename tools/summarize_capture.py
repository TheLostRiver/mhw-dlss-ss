"""Summarize real MhwSrProbe JSONL captures without asserting image quality or speed."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


def positive_int(value):
    return value if type(value) is int and value > 0 else None


def pair(row, width, height, texture=None):
    values = (positive_int(row.get(width)), positive_int(row.get(height)))
    if all(values):
        return values
    resource = row.get(texture) if texture else None
    if isinstance(resource, dict):
        values = (positive_int(resource.get("width")), positive_int(resource.get("height")))
        if all(values):
            return values
    return None


def summarize(path: Path) -> dict:
    records = []
    invalid_lines = []
    for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
            if not isinstance(record, dict):
                raise ValueError("not an object")
            records.append(record)
        except (json.JSONDecodeError, ValueError):
            invalid_lines.append(number)
    evaluations = [row for row in records if row.get("event") == "evaluate"]
    stage_records = [row for row in records if row.get("event") == "upstream_stage"]
    stage_observations = {}
    for row in stage_records:
        resource = row.get("source")
        key = json.dumps({
            "stage": row.get("stage"),
            "source": {key: resource.get(key) for key in ("width", "height", "format", "flags")}
                      if isinstance(resource, dict) else None,
            "context_size_candidate": row.get("context_size_candidate"),
            "viewport_method_address": row.get("viewport_method_address"),
        }, sort_keys=True)
        stage_observations[key] = stage_observations.get(key, 0) + 1
    signatures = {}
    jitter = set()
    for row in evaluations:
        active = pair(row, "active_width", "active_height")
        configured = pair(row, "render_width", "render_height")
        render = active or configured
        output = pair(row, "output_width", "output_height", "output")
        relation = "unknown"
        if render and output:
            if render == output:
                relation = "equal_input_output"
            elif all(i <= o for i, o in zip(render, output)):
                relation = "smaller_input_than_output"
            else:
                relation = "input_exceeds_output_on_at_least_one_axis"
        key = json.dumps({"input": render, "output": output, "relation": relation,
                          "quality": row.get("quality"), "flags": row.get("create_flags"),
                          "mv_scale": [row.get("mv_scale_x"), row.get("mv_scale_y")]}, sort_keys=True)
        signatures[key] = signatures.get(key, 0) + 1
        values = (row.get("jitter_x"), row.get("jitter_y"))
        if all(type(value) in (int, float) and math.isfinite(value) for value in values):
            jitter.add(values)
    texture_evidence = {}
    for name in ("color", "depth", "motion_vectors", "output", "exposure"):
        observed = {}
        for row in evaluations:
            resource = row.get(name)
            if isinstance(resource, dict):
                key = json.dumps({key: resource.get(key) for key in ("width", "height", "format", "dimension", "flags")}, sort_keys=True)
                observed[key] = observed.get(key, 0) + 1
        texture_evidence[name] = [{**json.loads(key), "samples": count} for key, count in observed.items()]
    return {
        "capture": str(path.resolve()), "invalid_json_lines": invalid_lines,
        "probe_sessions": sum(row.get("event") == "probe_start" for row in records),
        "attached_events": sum(row.get("event") == "attached" for row in records),
        "refusals": [row for row in records if row.get("event") == "refused"],
        "stage_hook_status": [row for row in records if row.get("event", "").startswith("stage_hooks_")],
        "upstream_stage_observations": [{**json.loads(key), "samples": count} for key, count in stage_observations.items()],
        "evaluate_samples": len(evaluations),
        "successful_evaluate_samples": sum(row.get("result") == 1 for row in evaluations),
        "dimension_observations": [{**json.loads(key), "samples": count} for key, count in signatures.items()],
        "distinct_jitter_samples": sorted(jitter), "resources": texture_evidence,
        "limitations": [
            "No GPU texture pixels were captured; correct depth and motion-vector content is not established.",
            "Upstream stage labels and context size candidates come from static analysis; this is not viewport capture.",
            "NGX input dimensions alone do not prove that the game rendered its scene at that lower resolution.",
            "No GPU timing or performance gain is measured.",
            "A sampled trace can miss transient failures and does not establish crash-free operation.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.dumps(summarize(args.capture), ensure_ascii=False, indent=2)
    if args.output:
        if args.output.resolve() == args.capture.resolve():
            parser.error("The summary must not overwrite the capture.")
        args.output.write_text(report + "\n", encoding="utf-8")
    print(report)


if __name__ == "__main__":
    main()
