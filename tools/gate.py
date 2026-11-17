#!/usr/bin/env python3
"""
Release-evidence gate for a candidate brake controller.

    python3 tools/gate.py out/sweep/summary.json [--expect FAIL] [--report-only]

Reads a bbw_sweep summary and gives one of three verdicts:

  PASS_WITHIN_SCOPE  every required check ran and nothing broke a limit.
                     This means "passed THIS test campaign" (these laps, these
                     condition ranges, this car model), not "safe".
  FAIL               the candidate broke a limit: any single 10 ms frame of a
                     hard limit (rear lock risk, braking demand not met), or a
                     soft limit (battery, lap energy, motor heat) for 50 ms.
  INVALID            the evidence itself cannot be trusted: missing or wrong
                     fields, wrong number of laps or frames, unknown classes,
                     or the trusted v1 broke a limit itself.

Fails closed: exit code 0 only for PASS_WITHIN_SCOPE (or, with --expect X,
only when the verdict is X, so CI can prove the gate rejects a known-bad
candidate). --report-only always exits 0.

Also writes <summary dir>/manifest.json: what exactly was tested (lap file,
monitor, controllers and binary hashes, seed, ranges, git commit), so a
verdict can be traced.

The monitor (the limit checks, src/monitor.c + include/monitor.h) is pinned:
its SHA-256 must match tools/monitor.sha256, or the verdict is INVALID. A
change to the checks therefore needs a deliberate, reviewable change to the
pin, and cannot slip in with a candidate.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

HARD = ["rear_lock_risk", "demand_not_met"]
SOFT = ["over_battery", "over_lap_cap", "over_motor"]
OTHER = ["agree", "within_limits", "shadow_missing"]
CLASSES = set(HARD + SOFT + OTHER)
REQUIRED = {"schema": int, "tool": str, "candidate": str, "mode": str, "seed": int, "lap_file": str,
            "lap_rows": int, "runs": int, "laps_per_run": int, "laps": int, "frames": int,
            "laps_with_unsafe_v2": int, "laps_with_hard_limit_break": int, "v1_limit_break_frames": int,
            "frames_by_class": dict, "episodes_by_class": dict, "peak_rear_grip_use": dict}


def sha256(path):
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fp:
            for chunk in iter(lambda: fp.read(1 << 16), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MONITOR_FILES = ["src/monitor.c", "include/monitor.h"]


def monitor_sha256():
    h = hashlib.sha256()
    for f in MONITOR_FILES:
        try:
            with open(os.path.join(ROOT, f), "rb") as fp:
                h.update(fp.read())
        except OSError:
            return None
    return h.hexdigest()


def pinned_monitor():
    path = os.environ.get("BBW_MONITOR_PIN", os.path.join(ROOT, "tools", "monitor.sha256"))
    try:
        return open(path).read().split()[0]
    except (OSError, IndexError):
        return None


def git_commit():
    if os.environ.get("GITHUB_SHA"):
        return os.environ["GITHUB_SHA"]
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
                              timeout=5).stdout.strip() or None
    except Exception:
        return None


def evaluate(s):
    """Return (verdict, reasons). Never raises on bad input: bad input is INVALID."""
    if not isinstance(s, dict):
        return "INVALID", ["summary is not a JSON object"]
    bad = []
    for k, t in REQUIRED.items():
        if k not in s:
            bad.append(f"missing field '{k}'")
        elif not isinstance(s[k], t) or (t is int and isinstance(s[k], bool)):
            bad.append(f"field '{k}' has the wrong type")
    if bad:
        return "INVALID", bad
    if s["schema"] != 2 or s["tool"] != "bbw_sweep":
        bad.append("not a bbw_sweep schema-2 summary")
    if s["mode"] not in ("shadow", "counterfactual"):
        bad.append(f"unknown mode '{s['mode']}'")
    if s["runs"] < 1 or s["laps_per_run"] < 1 or s["lap_rows"] < 10:
        bad.append("too few runs, laps or lap rows to mean anything")
    if s["laps"] != s["runs"] * s["laps_per_run"]:
        bad.append(f"expected {s['runs'] * s['laps_per_run']} laps, summary says {s['laps']}")
    if s["frames"] != s["laps"] * s["lap_rows"]:
        bad.append(f"expected {s['laps'] * s['lap_rows']} frames, summary says {s['frames']}")
    for key in ("frames_by_class", "episodes_by_class"):
        d = s[key]
        if set(d) != CLASSES:
            bad.append(f"{key} has unexpected or missing classes: {sorted(set(d) ^ CLASSES)}")
        elif any(not isinstance(v, int) or isinstance(v, bool) or v < 0 for v in d.values()):
            bad.append(f"{key} has a non-count value")
    if not bad and sum(s["frames_by_class"].values()) != s["frames"]:
        bad.append("frames by class do not add up to the frame count")
    if not bad and s["frames_by_class"]["shadow_missing"]:
        bad.append("candidate gave no valid answer on some frames")
    if s["v1_limit_break_frames"] != 0:
        bad.append(f"the trusted v1 itself broke a limit on {s['v1_limit_break_frames']} frames: "
                   "the comparison baseline is not valid")
    if bad:
        return "INVALID", bad

    fail = []
    ep, fr = s["episodes_by_class"], s["frames_by_class"]
    for c in HARD:
        if fr[c]:
            fail.append(f"hard limit '{c}' broken on {fr[c]} frame(s) ({ep[c]} event(s))")
    for c in SOFT:
        if ep[c]:
            fail.append(f"soft limit '{c}' broken for 50 ms or more {ep[c]} time(s)")
    if s["laps_with_unsafe_v2"]:
        fail.append(f"{s['laps_with_unsafe_v2']} of {s['laps']} laps had a limit break")
    return ("FAIL", fail) if fail else ("PASS_WITHIN_SCOPE", [])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("summary")
    ap.add_argument("--expect", choices=["PASS_WITHIN_SCOPE", "FAIL", "INVALID"],
                    help="exit 0 only if the verdict is this (CI self-test of the gate)")
    ap.add_argument("--report-only", action="store_true", help="always exit 0")
    ap.add_argument("--binary", default="build/host/bbw_sweep", help="tool binary to hash into the manifest")
    a = ap.parse_args()

    try:
        with open(a.summary) as fp:
            s = json.load(fp)
    except (OSError, ValueError) as e:
        s, load_err = None, str(e)
    else:
        load_err = None
    verdict, reasons = ("INVALID", [f"cannot read summary: {load_err}"]) if load_err else evaluate(s)
    mon, pin = monitor_sha256(), pinned_monitor()
    if mon is None or pin is None or mon != pin:
        verdict = "INVALID"
        reasons = reasons + [f"safety monitor does not match the pinned version (monitor {str(mon)[:12]}, "
                             f"pinned {str(pin)[:12]}): review the checks and update tools/monitor.sha256"]

    s = s if isinstance(s, dict) else {}
    name = s.get("candidate", "?")
    scope = (f"{s.get('laps', '?')} laps ({s.get('runs', '?')} runs x {s.get('laps_per_run', '?')}) of "
             f"{os.path.basename(str(s.get('lap_file', '?')))}, mode {s.get('mode', '?')}, seed {s.get('seed', '?')}; "
             f"battery 50-92%, air 10-45 C, grip 85-105%; modelled car (see docs)")
    lines = [f"## Shadow Lap release-evidence gate: {name}", "",
             f"**Verdict: {verdict}**", "", f"Scope: {scope}.", ""]
    if verdict == "PASS_WITHIN_SCOPE":
        lines.append("No limit was broken in this campaign. This is evidence for the next validation stage "
                     "(hardware-in-the-loop, then a supervised vehicle test), not proof of safety.")
    if reasons:
        lines += ["Reasons:", ""] + [f"- {r}" for r in reasons]
    ca = s.get("closest_approach") if isinstance(s.get("closest_approach"), dict) else {}
    cw = s.get("closest_approach_where") if isinstance(s.get("closest_approach_where"), dict) else {}
    if isinstance(ca.get("v2"), dict):
        names = [("rear_grip", "rear grip used"), ("battery", "battery charge limit used"),
                 ("motor", "motor limit used"), ("lap_energy", "lap energy limit used"),
                 ("demand_delivered", "braking demand delivered (lowest)")]
        lines += ["", f"Closest approach to each limit ({name} vs v1; 1.00 = at the limit):", "",
                  "| Limit | v1 | " + name + " | where (run, lap, lap time s) |", "|---|---|---|---|"]
        for k, label in names:
            w = cw.get(k)
            lines.append(f"| {label} | {ca.get('v1', {}).get(k, 0):.2f} | {ca['v2'].get(k, 0):.2f} | "
                         + (f"run {w[0]}, lap {w[1]}, {w[2]:.1f} s" if isinstance(w, list) and len(w) == 3 else "") + " |")
    pk = s.get("peak_rear_grip_use") if isinstance(s.get("peak_rear_grip_use"), dict) else {}
    if pk:
        lines += ["", f"Peak rear grip use: v1 {pk.get('v1', 0):.2f}, {name} {pk.get('v2', 0):.2f} (1.00 = at the limit)."]
    text = "\n".join(lines)
    print(text)

    manifest = {"verdict": verdict, "reasons": reasons, "candidate": name,
                "summary_file": a.summary, "summary_sha256": sha256(a.summary),
                "lap_file": s.get("lap_file"), "lap_sha256": sha256(s["lap_file"]) if s.get("lap_file") else None,
                "tool_binary": a.binary, "tool_sha256": sha256(a.binary),
                "monitor_sha256": mon, "monitor_pinned_sha256": pin,
                "controllers_sha256": sha256(os.path.join(ROOT, "src", "control.c")),
                "git_commit": git_commit(), "seed": s.get("seed"), "mode": s.get("mode"),
                "ranges": s.get("ranges"), "rules": s.get("rules")}
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(a.summary)), "manifest.json"), "w") as fp:
            json.dump(manifest, fp, indent=2)
    except OSError:
        pass
    gs = os.environ.get("GITHUB_STEP_SUMMARY")
    if gs:
        with open(gs, "a") as fp:
            fp.write(text + "\n\n")

    if a.report_only:
        sys.exit(0)
    if a.expect:
        sys.exit(0 if verdict == a.expect else 1)
    sys.exit(0 if verdict == "PASS_WITHIN_SCOPE" else 1)


if __name__ == "__main__":
    main()
