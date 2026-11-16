#!/usr/bin/env python3
"""The release gate must fail closed: bad, incomplete or unsafe evidence never passes."""
import copy
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from gate import evaluate  # noqa: E402

CLS = ["agree", "within_limits", "over_battery", "over_lap_cap", "rear_lock_risk",
       "over_motor", "demand_not_met", "shadow_missing"]
GOOD = {"schema": 2, "tool": "bbw_sweep", "candidate": "x", "mode": "shadow", "seed": 1,
        "lap_file": "data/lap_a.csv", "lap_rows": 100, "runs": 2, "laps_per_run": 3, "laps": 6,
        "frames": 600, "laps_with_unsafe_v2": 0, "laps_with_hard_limit_break": 0,
        "v1_limit_break_frames": 0, "peak_rear_grip_use": {"v1": 0.95, "v2": 0.95},
        "frames_by_class": {c: 0 for c in CLS}, "episodes_by_class": {c: 0 for c in CLS}}
GOOD["frames_by_class"]["agree"] = 600


def mut(fn):
    s = copy.deepcopy(GOOD)
    fn(s)
    return s


def drop(k):
    return lambda s: s.pop(k)


def setv(path, v):
    def f(s):
        d = s
        for k in path[:-1]:
            d = d[k]
        d[path[-1]] = v
    return f


def one_lock_frame(s):
    s["frames_by_class"]["agree"] -= 1
    s["frames_by_class"]["rear_lock_risk"] = 1


def short_soft(s):        # 4 frames of a soft limit: recorded, but not an episode
    s["frames_by_class"]["agree"] -= 4
    s["frames_by_class"]["over_battery"] = 4


def missing_answers(s):
    s["frames_by_class"]["agree"] -= 10
    s["frames_by_class"]["shadow_missing"] = 10


CASES = [
    ("good evidence", GOOD, "PASS_WITHIN_SCOPE"),
    ("one single frame of rear lock risk", mut(one_lock_frame), "FAIL"),
    ("soft limit shorter than 50 ms", mut(short_soft), "PASS_WITHIN_SCOPE"),
    ("soft limit episode", mut(lambda s: (short_soft(s), s["episodes_by_class"].update(over_battery=1))), "FAIL"),
    ("unsafe lap count", mut(setv(["laps_with_unsafe_v2"], 1)), "FAIL"),
    ("missing unsafe-lap field", mut(drop("laps_with_unsafe_v2")), "INVALID"),
    ("missing frame counts", mut(drop("frames_by_class")), "INVALID"),
    ("count as text", mut(setv(["laps_with_unsafe_v2"], "0")), "INVALID"),
    ("count as boolean", mut(setv(["laps_with_unsafe_v2"], False)), "INVALID"),
    ("fewer laps than runs x laps", mut(setv(["laps"], 5)), "INVALID"),
    ("frames do not add up", mut(setv(["frames_by_class", "agree"], 599)), "INVALID"),
    ("unknown class", mut(lambda s: s["frames_by_class"].update(new_thing=0)), "INVALID"),
    ("class missing", mut(lambda s: s["episodes_by_class"].pop("over_motor")), "INVALID"),
    ("negative count", mut(setv(["episodes_by_class", "over_motor"], -1)), "INVALID"),
    ("candidate missed frames", mut(missing_answers), "INVALID"),
    ("v1 broke a limit", mut(setv(["v1_limit_break_frames"], 3)), "INVALID"),
    ("old schema", mut(setv(["schema"], 1)), "INVALID"),
    ("not an object", [], "INVALID"),
]


def main():
    bad = 0
    for name, s, want in CASES:
        got, why = evaluate(s)
        ok = got == want
        bad += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {got}" + ("" if ok else f" (wanted {want}; {why})"))
    # the command line must fail closed too: exit code, unreadable and empty files
    with tempfile.TemporaryDirectory() as d:
        def run(content, *extra):
            p = os.path.join(d, "s.json")
            with open(p, "w") as fp:
                fp.write(content)
            return subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gate.py"), p, *extra],
                                  capture_output=True, text=True).returncode
        checks = [("exit 0 on pass", run(json.dumps(GOOD)), 0),
                  ("exit 1 on fail", run(json.dumps(mut(one_lock_frame))), 1),
                  ("exit 1 on empty file", run(""), 1),
                  ("exit 1 on broken JSON", run("{\"laps\": "), 1),
                  ("--expect FAIL accepts a fail", run(json.dumps(mut(one_lock_frame)), "--expect", "FAIL"), 0),
                  ("--expect FAIL rejects a pass", run(json.dumps(GOOD), "--expect", "FAIL"), 1)]
        for name, got, want in checks:
            ok = got == want
            bad += not ok
            print(f"{'ok  ' if ok else 'FAIL'} {name}: exit {got}")
    print("gate tests:", "all passed" if not bad else f"{bad} FAILED")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
