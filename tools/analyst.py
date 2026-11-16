#!/usr/bin/env python3
"""
AI analyst: turns a shadow run into an engineering verdict, with every
number checked against the data.

    python3 tools/analyst.py out/shadow                      # local Ollama (default)
    python3 tools/analyst.py out/shadow --model qwen2.5:7b
    python3 tools/analyst.py out/shadow --provider openai \
        --url https://generativelanguage.googleapis.com/v1beta/openai --model gemini-2.0-flash
        (key in the environment variable ANALYST_API_KEY)

How it stays honest:
  1. Rules (not the AI) find every disagreement episode, from compare.csv.
  2. The AI only sees that evidence, as JSON, and must cite episode ids.
  3. Every number the AI writes is checked against the evidence. Numbers that
     are not in the evidence are flagged as unverified in the output.
  4. The AI never touches the car. It runs after the lap, on the log.

Writes <run>/analyst.md and <run>/analyst.json. tools/report.py includes it.
Standard library only.
"""
import argparse
import json
import os
import re
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from report import load, episodes      # noqa: E402

SYSTEM = """You are a vehicle software validation engineer reviewing a shadow-mode test.
A trusted brake controller (v1) drove the car. A candidate controller (v2) saw the same inputs,
but its decisions were only logged. You are given rule-based evidence: disagreement episodes
with ids like E3 from one lap, and (if present) statistics from a sweep of many laps in varied
conditions. Battery, motor and brake forces come from a simulation model.

Write a short review in plain English for an engineering lead, with these sections:
## Verdict
One sentence: is v2 ready to control the car, yes or no, and the main reason.
## Problems found
One bullet per problem type, worst first. For each: what v2 did, where (cite episode ids),
why it matters physically, and the likely cause in v2's design.
## Fix before the next test
Concrete changes to v2's logic, one bullet each.
## Tests to run next
Two or three specific test conditions that would stress v2's weak points.

Rules: use only numbers that appear in the evidence, and copy them exactly. Cite episode ids
for every claim about the lap. Do not invent sensor data, laps or results. Keep it under 350 words."""


def build_evidence(run):
    d = load(os.path.join(run, "compare.csv"))
    summ = {}
    sj = os.path.join(run, "summary.json")
    if os.path.exists(sj):
        summ = json.load(open(sj))
    eps = episodes(d)
    out = []
    for i, e in enumerate(eps, 1):
        item = {
            "id": f"E{i}", "type": e["cls"],
            "braking_zone": e["zone"] + 1 if e["zone"] >= 0 else None,
            "t_start_s": round(e["t0"], 1), "t_end_s": round(e["t1"], 1), "frames": e["n"],
            "entry_speed_kmh": round(e["v0"]),
            "battery_pct_start": round(e["soc0"] * 100), "battery_pct_end": round(e["soc1"] * 100),
        }
        if e["cls"] == "rear_lock_risk":
            item.update(worst_v2_rear_kN=round(e["v2rear"] / 1e3, 1), rear_grip_kN=round(e["grip"] / 1e3, 1),
                        worst_at_s=round(e["t_lock"], 1), worst_speed_kmh=round(e["v_lock"]))
        elif e["cls"] == "over_battery":
            item.update(v1_charge_kW=round(e["p1"]), v2_charge_kW=round(e["p2"]),
                        at_s=round(e["t_pow"], 1), battery_pct_then=round(e["soc_k"] * 100))
        elif e["cls"] == "over_lap_cap":
            item.update(lap_harvest_used_MJ=round(e["harvest"], 2), v2_max_charge_kW=round(e["p2max"]))
        elif e["cls"] == "over_motor":
            item.update(v2_max_charge_kW=round(e["p2max"]))
        elif e["cls"] == "demand_not_met":
            item.update(rear_asked_kN=round(e["req"] / 1e3, 1), v2_rear_delivered_kN=round(e["v2short"] / 1e3, 1))
        out.append(item)
    counts = {}
    for c in d["class"]:
        counts[str(c)] = counts.get(str(c), 0) + 1
    many = None
    sp = os.path.join(os.path.dirname(os.path.normpath(run)), "sweep", "summary.json")
    if os.path.exists(sp):
        sm = json.load(open(sp))
        many = {k: sm.get(k) for k in ("candidate", "mode", "laps", "laps_with_unsafe_v2", "laps_with_hard_limit_break",
                                       "laps_with_demand_not_met", "v1_limit_break_frames", "laps_with_lock_while_wings_close",
                                       "laps_with_lock_low_grip", "laps_with_hot_motor_overreach",
                                       "v1_harvest_mj_per_lap", "v2_safe_harvest_mj_per_lap")}
    return {
        "many_laps_sweep": many,
        "run": os.path.basename(os.path.normpath(run)),
        "frames": int(len(d)),
        "frame_counts_by_class": counts,
        "v1_deadline_misses": summ.get("v1_deadline_misses"),
        "plant_overruns": summ.get("plant_overruns"),
        "v2_design_notes": [
            "v2 moves 6% more braking to the rear axle to harvest more",
            "v2 charges at full power until the battery is 90% full; v1 tapers from 80%",
            "v2 has no lap-harvest-limit check, no rear-grip check and no motor-temperature check",
            "the rear wings take about 0.2 s to close when braking starts, so grip is lower at first",
        ],
        "model_limits": {"motor_max_kW": 350, "lap_harvest_cap_MJ": 2.4, "battery_taper_start_pct": 80},
        "episodes": out,
    }


def call_ollama(url, model, system, user, timeout):
    body = json.dumps({"model": model, "stream": False, "options": {"temperature": 0.2},
                       "messages": [{"role": "system", "content": system},
                                    {"role": "user", "content": user}]}).encode()
    req = urllib.request.Request(url.rstrip("/") + "/api/chat", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)["message"]["content"]


def call_openai(url, model, system, user, timeout):
    key = os.environ.get("ANALYST_API_KEY", "")
    body = json.dumps({"model": model, "temperature": 0.2,
                       "messages": [{"role": "system", "content": system},
                                    {"role": "user", "content": user}]}).encode()
    req = urllib.request.Request(url.rstrip("/") + "/chat/completions", data=body,
                                 headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)["choices"][0]["message"]["content"]


def evidence_numbers(ev):
    nums = set()

    def walk(x):
        if isinstance(x, bool):
            return
        if isinstance(x, (int, float)):
            nums.add(float(x))
        elif isinstance(x, dict):
            for k, v in x.items():
                walk(v)
                m = re.search(r"\d+(\.\d+)?", k)
                if m and k.startswith("E"):
                    nums.add(float(m.group()))
        elif isinstance(x, list):
            for v in x:
                walk(v)
        elif isinstance(x, str):
            for m in re.finditer(r"\d+(?:\.\d+)?", x):
                nums.add(float(m.group()))
    walk(ev)
    return nums


def check_numbers(text, ev):
    """Return numbers in the AI text that do not appear in the evidence."""
    known = evidence_numbers(ev)
    bad = []
    for m in re.finditer(r"(?<![A-Za-z\d.])(\d+(?:\.\d+)?)", text):
        tok = m.group(1)
        before = text[max(0, m.start() - 1):m.start()]
        if before == "E":                     # episode id
            continue
        v = float(tok)
        if v in (1, 2, 3, 4, 5) and "." not in tok:   # list markers, counts of things
            continue
        if any(abs(v - k) <= max(0.051, 0.006 * abs(k)) for k in known):
            continue
        bad.append(tok)
    return sorted(set(bad), key=lambda s: float(s))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run", nargs="?", default="out/shadow")
    ap.add_argument("--provider", choices=["ollama", "openai"], default=os.environ.get("ANALYST_PROVIDER", "ollama"))
    ap.add_argument("--url", default=os.environ.get("ANALYST_URL", "http://localhost:11434"))
    ap.add_argument("--model", default=os.environ.get("ANALYST_MODEL", "qwen2.5:7b"))
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--evidence-only", action="store_true", help="write the evidence, skip the AI")
    a = ap.parse_args()

    ev = build_evidence(a.run)
    json.dump(ev, open(os.path.join(a.run, "analyst_evidence.json"), "w"), indent=2)
    print(f"evidence: {len(ev['episodes'])} episodes -> {a.run}/analyst_evidence.json")
    if a.evidence_only:
        return

    user = "Evidence (JSON):\n" + json.dumps(ev, indent=1)
    try:
        fn = call_ollama if a.provider == "ollama" else call_openai
        text = fn(a.url, a.model, SYSTEM, user, a.timeout)
    except Exception as e:                       # noqa: BLE001
        print(f"AI call failed ({a.provider} at {a.url}, model {a.model}): {e}")
        print("The rule-based findings in the report are unaffected. See docs/ai-analyst.md.")
        sys.exit(2)

    unverified = check_numbers(text, ev)
    cited = sorted(set(re.findall(r"\bE\d+\b", text)), key=lambda s: int(s[1:]))
    known_ids = {e["id"] for e in ev["episodes"]}
    bad_ids = [c for c in cited if c not in known_ids]

    footer = ["", "---", f"*Written by {a.model} from rule-based evidence; checked automatically.*"]
    footer.append(f"*Episodes cited: {', '.join(cited) or 'none'}.*")
    if unverified:
        footer.append(f"*Unverified numbers (not in the evidence): {', '.join(unverified)}.*")
    else:
        footer.append("*Every number above appears in the evidence.*")
    if bad_ids:
        footer.append(f"*Unknown episode ids: {', '.join(bad_ids)}.*")
    md = text.strip() + "\n" + "\n".join(footer) + "\n"
    open(os.path.join(a.run, "analyst.md"), "w").write(md)
    json.dump({"model": a.model, "provider": a.provider, "text": text, "cited": cited,
               "unverified_numbers": unverified, "unknown_ids": bad_ids},
              open(os.path.join(a.run, "analyst.json"), "w"), indent=2)
    print(md)
    print(f"wrote {a.run}/analyst.md  (unverified numbers: {len(unverified)}, unknown ids: {len(bad_ids)})")


if __name__ == "__main__":
    main()
