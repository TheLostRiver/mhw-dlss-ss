"""Summarize saved standalone capture logs without opening the game or GPU."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def summarize(path: Path) -> dict:
    rows = []
    incomplete = 0
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            incomplete += 1
            continue
        if isinstance(item, dict):
            rows.append(item)
    events = lambda name: [row for row in rows if row.get("event") == name]
    samples = events("native_taa_input")
    summaries = events("native_input_summary")
    projection = events("native_projection_summary")
    session_ids = sorted({row.get("session", 1) for row in samples + summaries + projection})
    sessions = []
    for session in session_ids:
        inputs = [row for row in samples if row.get("session", 1) == session]
        valid = [row for row in inputs if row.get("valid")]
        adjacent = [(a, b) for a, b in zip(inputs, inputs[1:])
                    if a.get("valid") and b.get("valid") and b.get("serial", 0) == a.get("serial", 0) + 1]
        matches = sum(all(abs(a["projection_jitter"][i] - b["previous_projection_jitter"][i]) <= 1e-8
                          for i in (0, 1)) for a, b in adjacent)
        counts = [row for row in summaries if row.get("session", 1) == session]
        jitter = [row for row in projection if row.get("session", 1) == session]
        sessions.append({
            "session": session,
            "runtime_counts": counts[-1] if counts else None,
            "projection_counts": jitter[-1] if jitter else None,
            "logged_input_records": len(inputs),
            "logged_valid_records": len(valid),
            "logged_adjacent_pairs": len(adjacent),
            "logged_previous_matches_prior_current": matches,
            "logged_output_sizes": sorted({tuple(row["output"]) for row in valid}),
            "logged_input_sizes": sorted({tuple(row["input"]) for row in valid}),
            "logged_distinct_current_jitter": sorted({tuple(row["projection_jitter"]) for row in valid}),
            "logged_phases": sorted({row.get("phase", "unspecified") for row in inputs}),
        })
    return {
        "event_count": len(rows),
        "incomplete_or_invalid_lines": incomplete,
        "startup": events("native_host_starting"),
        "disabled": events("native_host_disabled"),
        "space_reservations": events("native_projection_space_reserved"),
        "projection_wait": events("native_projection_site_waiting"),
        "projection_ready": events("native_projection_site_ready"),
        "ready": events("native_host_ready"),
        "refusals": events("native_host_refused"),
        "stopped": events("native_host_stopped"),
        "sessions": sessions,
        "sr_execution_observed": any(row.get("sr_execution") is True for row in rows),
        "limits": ["Logged input records can be sparse; use runtime_counts for total input counts.",
                   "Jitter and history matches are parameter evidence, not image quality or performance validation.",
                   "A ready log alone does not prove the process is still running."],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output and args.output.resolve() == args.log.resolve():
        parser.error("Output cannot replace input.")
    value = json.dumps(summarize(args.log), ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(value + "\n", encoding="utf-8")
    print(value)


if __name__ == "__main__":
    main()
