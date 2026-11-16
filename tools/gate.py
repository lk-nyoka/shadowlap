#!/usr/bin/env python3
"""
Release gate: decide from the shadow evidence whether v2 may replace v1.

    python3 tools/gate.py out/sweep/summary.json [--strict]

Prints a verdict (and writes it to the GitHub job summary when run in
GitHub Actions). Exit code is 0 unless --strict is given and v2 is rejected,
so the repo's checks stay green while still showing the verdict; use --strict
in a team's real pipeline to block the release.
"""
import argparse
import json
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("summary")
    ap.add_argument("--strict", action="store_true", help="exit 1 when v2 is rejected")
    ap.add_argument("--max-unsafe-laps", type=int, default=0)
    a = ap.parse_args()

    s = json.load(open(a.summary))
    laps = s.get("laps", 0)
    unsafe = s.get("laps_with_unsafe_v2", 0)
    reasons = []
    if s.get("laps_with_lock_while_wings_close"):
        reasons.append(f"rear lock risk while the wings close on {s['laps_with_lock_while_wings_close']} of {laps} laps")
    if s.get("laps_with_lock_low_grip"):
        reasons.append(f"rear lock risk on low-grip tracks on {s['laps_with_lock_low_grip']} of {laps} laps")
    if s.get("laps_with_hot_motor_overreach"):
        reasons.append(f"over-asks a hot motor on {s['laps_with_hot_motor_overreach']} of {laps} laps")
    ep = s.get("episodes_by_class", {})
    if ep.get("over_battery"):
        reasons.append(f"{ep['over_battery']} battery over-charge episodes")
    if ep.get("over_lap_cap"):
        reasons.append(f"{ep['over_lap_cap']} lap-energy-limit episodes")

    rejected = unsafe > a.max_unsafe_laps
    name = s.get("candidate", "v2")
    v1 = s.get("v1_harvest_mj_per_lap", 0)
    v2 = s.get("v2_safe_harvest_mj_per_lap", 0)
    lines = [
        f"## Shadow Lap release gate: {name}",
        "",
        f"**Verdict: candidate {name} is {'REJECTED' if rejected else 'APPROVED'}** "
        f"({unsafe} of {laps} laps had an unsafe decision lasting 50 ms or more; allowed: {a.max_unsafe_laps}).",
        "",
    ]
    if reasons:
        lines += ["Why:", ""] + [f"- {r}" for r in reasons] + [""]
    lines.append(f"Energy per lap: v1 {v1:.2f} MJ; {name} where safe {v2:.2f} MJ.")
    text = "\n".join(lines)
    print(text)
    gs = os.environ.get("GITHUB_STEP_SUMMARY")
    if gs:
        with open(gs, "a") as fp:
            fp.write(text + "\n")
    sys.exit(1 if (rejected and a.strict) else 0)


if __name__ == "__main__":
    main()
