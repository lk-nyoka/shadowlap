#!/usr/bin/env python3
"""
Build a lap from a video's braking moments, so the charts match the video.

You mark when the car brakes in the video (the dashboard's "Calibrate" button
does this while you watch). This turns those moments into a 100 Hz lap file:
braking exactly when the video brakes, accelerating in between.

What comes from the video: when braking starts and stops, and the lap length.
What is modelled: the speeds (from typical acceleration and braking for a
fast car, between a minimum and maximum speed you can set).

    python3 tools/lapgen.py --duration 72.4 --brakes "8.1-10.3,21.5-23.0" --out data/lap_video.csv
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_synthetic_lap import accel, MASS, G, MU, DOWNFORCE_K   # noqa: E402

HZ = 100


def build(duration, brakes, v_start_kmh=250.0, v_max_kmh=320.0, v_min_kmh=70.0, effort=0.85):
    """brakes: list of (start_s, end_s) relative to the lap start."""
    brakes = sorted((max(0.0, a), min(duration, b)) for a, b in brakes if b > a)
    n = int(round(duration * HZ)) + 1
    v = v_start_kmh / 3.6
    vmax, vmin = v_max_kmh / 3.6, v_min_kmh / 3.6
    dist = 0.0
    rows = []
    zi = 0
    for k in range(n):
        t = k / HZ
        while zi < len(brakes) and t >= brakes[zi][1]:
            zi += 1
        braking = zi < len(brakes) and brakes[zi][0] <= t < brakes[zi][1]
        zone = zi if braking else -1
        if braking:
            a = -effort * MU * (G + DOWNFORCE_K * v * v / MASS)
            thr = 0.0
        else:
            a = accel(v) if v < vmax else 0.0
            thr = 1.0 if a > 0.5 else 0.35
        rows.append((t, dist, v * 3.6, thr, 1 if braking and v > vmin else 0, zone if v > vmin else -1))
        v = min(vmax, max(vmin, v + a / HZ))
        dist += v / HZ
    return rows


def write(rows, path):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as fp:
        fp.write("t,dist_m,speed_kmh,throttle,brake,zone\n")
        for r in rows:
            fp.write("%.3f,%.1f,%.2f,%.2f,%d,%d\n" % r)


def parse_brakes(s):
    out = []
    for part in s.split(","):
        part = part.strip()
        if part:
            a, b = part.split("-")
            out.append((float(a), float(b)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=float, required=True, help="lap length in seconds")
    ap.add_argument("--brakes", required=True, help='e.g. "8.1-10.3,21.5-23.0" (seconds from lap start)')
    ap.add_argument("--v-start", type=float, default=250)
    ap.add_argument("--v-max", type=float, default=320)
    ap.add_argument("--v-min", type=float, default=70)
    ap.add_argument("--out", default="data/lap_video.csv")
    a = ap.parse_args()
    rows = build(a.duration, parse_brakes(a.brakes), a.v_start, a.v_max, a.v_min)
    write(rows, a.out)
    print(json.dumps({"out": a.out, "samples": len(rows)}))


if __name__ == "__main__":
    main()
