#!/usr/bin/env python3
"""
Fetch a real lap from OpenF1 (free, no account needed for past sessions):
real speed, throttle, brake and the car's position on track.

    python3 tools/fetch_openf1_lap.py --year 2024 --circuit Monza --session Qualifying
    python3 tools/fetch_openf1_lap.py --year 2024 --circuit Monza --session Qualifying --driver 16

Writes data/lap_openf1_<circuit>_<year>_<driver>.csv in the usual lap format
(t,dist_m,speed_kmh,throttle,brake,zone) plus x,y columns for the track map,
and a .json with what the lap is. Public timing data is sampled about
4 times a second; this interpolates to 100 Hz.

What is real: speed, throttle, brake on/off, track position, lap time.
What is modelled: everything inside the car (battery, motor, brake forces).

Data via OpenF1 (https://openf1.org), an unofficial project not associated
with the Formula 1 companies, intended for education, research and
non-commercial use. F1 and related marks are trademarks of Formula One
Licensing B.V. The fetched files are not committed to this repo (see
.gitignore); each user downloads their own copy.
"""
import argparse
import datetime as dt
import json
import os
import urllib.parse
import urllib.request

import numpy as np

API = "https://api.openf1.org/v1/"


def get(endpoint, **params):
    q = "&".join(f"{k}={urllib.parse.quote(str(v), safe='<>=:-.T')}" for k, v in params.items())
    with urllib.request.urlopen(API + endpoint + "?" + q, timeout=60) as r:
        return json.load(r)


def parse_time(s):
    return dt.datetime.fromisoformat(s.replace("Z", "+00:00"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--year", type=int, default=2024)
    ap.add_argument("--circuit", default="Monza", help="OpenF1 circuit_short_name, e.g. Monza, Silverstone, Spa-Francorchamps")
    ap.add_argument("--session", default="Qualifying")
    ap.add_argument("--driver", type=int, default=None, help="car number; default: the session's fastest lap")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    sessions = get("sessions", year=a.year, circuit_short_name=a.circuit, session_name=a.session)
    if not sessions:
        raise SystemExit(f"no {a.session} session found for {a.circuit} {a.year}")
    s = sessions[0]
    key = s["session_key"]
    laps = [lap for lap in get("laps", session_key=key) if lap.get("lap_duration") and lap.get("date_start")]
    if a.driver:
        laps = [lap for lap in laps if lap["driver_number"] == a.driver]
    if not laps:
        raise SystemExit("no timed laps found")
    best = min(laps, key=lambda lap: lap["lap_duration"])
    drv, dur = best["driver_number"], best["lap_duration"]
    t0 = parse_time(best["date_start"])
    t1 = t0 + dt.timedelta(seconds=dur)
    lo = (t0 - dt.timedelta(seconds=2)).strftime("%Y-%m-%dT%H:%M:%S")
    hi = (t1 + dt.timedelta(seconds=2)).strftime("%Y-%m-%dT%H:%M:%S")
    car = get("car_data", session_key=key, driver_number=drv, **{"date>": lo, "date<": hi})
    loc = get("location", session_key=key, driver_number=drv, **{"date>": lo, "date<": hi})
    if len(car) < 20 or len(loc) < 20:
        raise SystemExit("not enough telemetry returned for that lap")

    def secs(rows):
        return np.array([(parse_time(r["date"]) - t0).total_seconds() for r in rows])

    tc, tl = secs(car), secs(loc)
    t = np.arange(0, dur, 0.01)
    spd = np.interp(t, tc, [r["speed"] for r in car])
    # public data comes ~4 times a second: smooth over 0.25 s so the braking
    # rate (deceleration) isn't made of steps between samples
    win = 25
    spd = np.convolve(np.pad(spd, (win // 2, win - 1 - win // 2), mode="edge"), np.ones(win) / win, mode="valid")
    thr = np.clip(np.interp(t, tc, [r["throttle"] for r in car]) / 100.0, 0, 1)
    brk_raw = np.interp(t, tc, [1.0 if (r.get("brake") or 0) > 0 else 0.0 for r in car])
    x = np.interp(t, tl, [r["x"] for r in loc])
    y = np.interp(t, tl, [r["y"] for r in loc])

    # braking = brake pedal on AND the car is actually slowing
    dv = np.gradient(spd) * 100.0 / 3.6
    brk = (brk_raw > 0.5) & (dv < -2.0)
    dist = np.concatenate([[0.0], np.cumsum(spd[1:] / 3.6 * 0.01)])
    zone = np.full(len(t), -1)
    z = -1
    for i in range(len(t)):
        if brk[i] and (i == 0 or not brk[i - 1]):
            z += 1
        if brk[i]:
            zone[i] = z

    name = f"lap_openf1_{a.circuit.lower().replace(' ', '_')}_{a.year}_{drv}"
    out = a.out or os.path.join("data", name + ".csv")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w") as fp:
        fp.write("t,dist_m,speed_kmh,throttle,brake,zone,x,y\n")
        for row in zip(t, dist, spd, thr, brk.astype(int), zone, x, y):
            fp.write("%.3f,%.1f,%.2f,%.2f,%d,%d,%.0f,%.0f\n" % row)
    m, sec = divmod(dur, 60)
    meta = {"label": f"Real lap: {a.circuit} {a.year} {a.session}, car {drv}, {int(m)}:{sec:06.3f}",
            "source": "OpenF1 (unofficial, non-commercial use; https://openf1.org)",
            "sampling": "about 3.7 Hz, interpolated to 100 Hz; speed smoothed over 250 ms", "session_key": key, "driver_number": drv,
            "lap_number": best.get("lap_number"), "lap_time_s": dur, "circuit": a.circuit, "year": a.year,
            "real": ["speed", "throttle", "brake on/off", "track position", "lap time"],
            "modelled": ["battery", "motor", "brake forces", "wings", "grip"]}
    json.dump(meta, open(out[:-4] + ".json", "w"), indent=1)
    print(f"wrote {out}: {meta['label']}, {len(car)} telemetry samples -> {len(t)} at 100 Hz, "
          f"{z + 1} braking zones, top {spd.max():.0f} km/h")
    print("Data via OpenF1 (unofficial; non-commercial use). F1 marks belong to Formula One Licensing B.V.")


if __name__ == "__main__":
    main()
