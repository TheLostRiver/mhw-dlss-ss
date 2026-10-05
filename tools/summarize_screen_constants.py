"""Summarize bounded CBScreen layout candidates; snapshots are CPU observations, not GPU execution proof."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def summarize(path):
    raw = path.read_bytes()
    sessions, arenas, versions = {}, [], []
    run, version, malformed = 0, None, 0
    for line in raw.decode("utf-8-sig").splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            malformed += 1
            continue
        event = item.get("event")
        if event == "attached":
            run += 1
            version = item.get("version")
            versions.append(version)
        if event == "arena_description":
            arenas.append({"run": run, "version": version, **item})
        if event not in ("armed", "screen_constants", "rejected_block", "capture_complete"):
            continue
        key = (run, item["session"])
        session = sessions.setdefault(key, {"run": run, "version": version, "session": item["session"],
                                           "candidates": [], "rejected": [], "completion": None})
        if event == "armed":
            session["armed"] = item
        elif event == "capture_complete":
            session["completion"] = item
        elif event == "screen_constants":
            session["candidates"].append(item)
        else:
            session["rejected"].append(item)

    results = []
    group_fields = ("screen_size", "view_size_uint", "view_size_float", "scales")
    for session in sessions.values():
        groups = {}
        for record in session.pop("candidates"):
            dimensions = {name: record[name] for name in group_fields}
            key = json.dumps(dimensions, sort_keys=True)
            group = groups.setdefault(key, {**dimensions, "unique_logged_snapshots": 0, "bindings": set(),
                                            "observed_viewports": set(), "example": record})
            group["unique_logged_snapshots"] += 1
            group["bindings"].add((record["stage"], record["root"]))
            viewport = record.get("last_observed_viewport")
            if viewport is not None:
                group["observed_viewports"].add(tuple(viewport))
        for group in groups.values():
            group["bindings"] = [list(pair) for pair in sorted(group["bindings"])]
            group["observed_viewports"] = [list(value) for value in sorted(group["observed_viewports"])]
        session["dimension_and_scale_groups"] = sorted(groups.values(), key=lambda group: -group["unique_logged_snapshots"])
        rejected = session.pop("rejected")
        session["rejected_diagnostic_counts"] = dict(Counter(str(record["first_failed_invariant"]) for record in rejected))
        results.append(session)
    return {"source": str(path.resolve()), "sha256": hashlib.sha256(raw).hexdigest(), "versions": versions,
            "malformed_lines": malformed, "arena_descriptions": arenas, "sessions": results,
            "limitations": ["Observed CPU contents at CBV binding, before GPU submission.",
                            "Layout matches are candidates; ring-buffer reuse can leave stale fields.",
                            "Snapshot counts are deduplicated and sampled, not draw-call or frame frequencies.",
                            "MHWSS upscaler markers come from its configuration file, not runtime feature execution."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = summarize(args.log)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    for session in report["sessions"]:
        arm, completion = session.get("armed", {}), session["completion"] or {}
        print(json.dumps({"version": session["version"], "session": session["session"],
                          "setting": arm.get("setting"), "upscaler_config": arm.get("mhwss_upscaler_config"),
                          "complete": session["completion"] is not None,
                          "dimension_groups": len(session["dimension_and_scale_groups"]),
                          "unique_blocks": completion.get("unique_blocks"), "dropped": completion.get("dropped")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
