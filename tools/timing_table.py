#!/usr/bin/env python3
"""Summarise repeated runs from scripts/repeat.sh: spread of v1 timing per scenario.

    python3 tools/timing_table.py out/repeat

Prints a Markdown table: runs, total v1 misses, host stalls (min/median/max),
v1 p99 and worst response (median and max), and the guard's containment time.
"""
import glob
import json
import os
import re
import statistics
import sys


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "out/repeat"
    rows = {}
    for sj in sorted(glob.glob(os.path.join(root, "run*", "*", "summary.json"))):
        scen = os.path.basename(os.path.dirname(sj))
        try:
            s = json.load(open(sj))
        except ValueError:
            continue
        r = rows.setdefault(scen, {"miss": [], "stall": [], "p99": [], "max": [], "contain": []})
        r["miss"].append(s.get("v1_deadline_misses", 0))
        r["stall"].append(s.get("plant_overruns", 0))
        r["p99"].append(s.get("v1_latency_us", {}).get("p99", float("nan")))
        r["max"].append(s.get("v1_latency_us", {}).get("max", float("nan")))
        g = os.path.join(os.path.dirname(sj), "guard.log")
        if os.path.exists(g):
            m = re.search(r"ran hot for (\d+) ms", open(g).read())
            if m:
                r["contain"].append(int(m.group(1)))
    if not rows:
        sys.exit(f"no runs found under {root}/run*/<scenario>/summary.json")
    med = statistics.median
    print("| Scenario | Runs | v1 misses (total) | Host stalls min / median / max | v1 p99 median (µs) | "
          "v1 worst median / max (ms) | Guard containment (ms) |")
    print("|---|---|---|---|---|---|---|")
    for scen, r in rows.items():
        c = (f"{min(r['contain'])}–{max(r['contain'])}" if r["contain"] else "")
        print(f"| {scen} | {len(r['miss'])} | {sum(r['miss'])} | {min(r['stall'])} / {med(r['stall']):.0f} / "
              f"{max(r['stall'])} | {med(r['p99']):.0f} | {med(r['max']) / 1000:.2f} / {max(r['max']) / 1000:.2f} | {c} |")


if __name__ == "__main__":
    main()
