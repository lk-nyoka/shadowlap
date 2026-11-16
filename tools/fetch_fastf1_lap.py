#!/usr/bin/env python3
"""
Turn a real lap from FastF1 into the same CSV format as the synthetic lap.

    pip install fastf1
    python tools/fetch_fastf1_lap.py --year 2024 --event Monza --session Q --out data/lap_real.csv

What the public data gives you: speed, throttle, gear and a brake on/off flag,
sampled every few hundred milliseconds. This script interpolates it to 100 Hz.
Everything inside the car (battery, motor, brake forces) comes from the model
in src/control.c, so results built on this lap are model outputs.

FastF1 3.9 added login with an F1TV account because Formula 1 started
requiring authentication on parts of its timing service. If loading fails with
"data not loaded", see FastF1's "F1TV Account Authentication" docs
(`python -m fastf1 auth f1tv --authenticate`). Not tested by us yet.

Check FastF1's and the data provider's terms before redistributing the CSV.
The repo ships only the synthetic lap for that reason.
"""
import argparse
import os
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--year", type=int, default=2024)
    ap.add_argument("--event", default="Monza")
    ap.add_argument("--session", default="Q")
    ap.add_argument("--driver", default=None, help="three-letter code; default fastest lap overall")
    ap.add_argument("--out", default="data/lap_real.csv")
    ap.add_argument("--cache", default=os.path.expanduser("~/.cache/fastf1"))
    a = ap.parse_args()

    import fastf1
    os.makedirs(a.cache, exist_ok=True)
    fastf1.Cache.enable_cache(a.cache)
    s = fastf1.get_session(a.year, a.event, a.session)
    s.load(laps=True, telemetry=True, weather=False, messages=False)
    laps = s.laps
    if a.driver:
        laps = laps.pick_drivers(a.driver) if hasattr(laps, "pick_drivers") else laps.pick_driver(a.driver)
    lap = laps.pick_fastest()
    tel = lap.get_car_data().add_distance()

    t_src = tel["Time"].dt.total_seconds().to_numpy()
    t_src -= t_src[0]
    hz = 100
    t = np.arange(0, t_src[-1], 1.0 / hz)
    kmh = np.interp(t, t_src, tel["Speed"].to_numpy(dtype=float))
    dist = np.interp(t, t_src, tel["Distance"].to_numpy(dtype=float))
    thr = np.clip(np.interp(t, t_src, tel["Throttle"].to_numpy(dtype=float)) / 100.0, 0, 1)
    brk = np.interp(t, t_src, tel["Brake"].to_numpy(dtype=float)) > 0.5

    zone = np.full(len(t), -1)
    z = -1
    for i in range(len(t)):
        if brk[i] and (i == 0 or not brk[i - 1]):
            z += 1
        if brk[i]:
            zone[i] = z

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "w") as fp:
        fp.write("t,dist_m,speed_kmh,throttle,brake,zone\n")
        for row in zip(t, dist, kmh, thr, brk.astype(int), zone):
            fp.write("%.3f,%.1f,%.2f,%.2f,%d,%d\n" % row)
    print(f"wrote {a.out}: {len(t)} samples (interpolated from {len(t_src)}), "
          f"lap {t[-1]:.1f}s, {z + 1} braking zones")


if __name__ == "__main__":
    main()
