#!/usr/bin/env python3
"""
Generate "Track A": a synthetic lap loosely shaped like a fast, low-downforce
circuit (long straights, chicanes). It is NOT real telemetry. It exists so the
repo runs out of the box with no downloads and no third-party data.

Output CSV at 100 Hz: t,dist_m,speed_kmh,throttle,brake,zone
"""
import argparse
import numpy as np

# (apex distance m, apex speed km/h, half-width m, how close to the grip
#  limit the driver brakes for this corner: 1.0 = right on the limit)
CORNERS = [
    (900, 80, 40, 0.93),     # chicane after the main straight: on the limit
    (1900, 250, 80, 0.80),   # fast right-hander
    (2300, 95, 40, 0.84),    # second chicane
    (2850, 180, 50, 0.86),
    (3150, 170, 50, 0.92),
    (3950, 160, 60, 0.84),
    (5000, 200, 120, 0.82),  # long final corner
]
# must match src/control.h
MASS, G, MU, DOWNFORCE_K = 800.0, 9.81, 1.6, 2.0
LAP_M = 5793


def accel(v):           # m/s^2 available for speeding up at speed v (m/s)
    return min(12.0, 650e3 / (800 * max(v, 10.0))) - 1.2e-3 * v * v


def decel(v, frac):     # m/s^2 the driver uses when braking at speed v
    return frac * MU * (G + DOWNFORCE_K * v * v / MASS)


def build(hz=100):
    ds = 1.0
    x = np.arange(0, LAP_M, ds)
    cap = np.full_like(x, 95.0)                       # top-speed cap, m/s
    frac = np.zeros_like(x)                           # braking effort toward next corner
    for d, kmh, w, f in CORNERS:
        m = (x > d - w) & (x < d + w)
        cap[m] = np.minimum(cap[m], kmh / 3.6)
    for i, xi in enumerate(x):
        ahead = [c for c in CORNERS if c[0] >= xi] or CORNERS[:1]
        frac[i] = ahead[0][3]

    # forward pass (accelerate), backward pass (brake), run twice for wrap-around
    v = cap.copy()
    for _ in range(2):
        for i in range(1, len(x)):
            v[i] = min(v[i], np.sqrt(v[i - 1] ** 2 + 2 * accel(v[i - 1]) * ds))
        v[0] = min(v[0], np.sqrt(v[-1] ** 2 + 2 * accel(v[-1]) * ds))
    for _ in range(2):
        for i in range(len(x) - 2, -1, -1):
            v[i] = min(v[i], np.sqrt(v[i + 1] ** 2 + 2 * decel(v[i + 1], frac[i]) * ds))
        v[-1] = min(v[-1], np.sqrt(v[0] ** 2 + 2 * decel(v[0], frac[0]) * ds))

    # distance -> time
    dt_seg = ds / np.maximum(v, 1.0)
    t_of_x = np.concatenate([[0.0], np.cumsum(dt_seg)[:-1]])
    t = np.arange(0, t_of_x[-1], 1.0 / hz)
    dist = np.interp(t, t_of_x, x)
    spd = np.interp(t, t_of_x, v)

    dv = np.gradient(spd) * hz
    brake = (dv < -3.0).astype(int)
    throttle = np.where(dv > 0.5, 1.0, np.where(brake == 1, 0.0, 0.35))

    zone = np.full(len(t), -1)
    z = -1
    for i in range(len(t)):
        if brake[i] and (i == 0 or not brake[i - 1]):
            z += 1
        if brake[i]:
            zone[i] = z
    return t, dist, spd * 3.6, throttle, brake, zone


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data/lap_a.csv")
    a = ap.parse_args()
    t, d, kmh, thr, br, zone = build()
    with open(a.out, "w") as fp:
        fp.write("t,dist_m,speed_kmh,throttle,brake,zone\n")
        for row in zip(t, d, kmh, thr, br, zone):
            fp.write("%.3f,%.1f,%.2f,%.2f,%d,%d\n" % row)
    print(f"wrote {a.out}: {len(t)} samples, lap {t[-1]:.1f}s, top {kmh.max():.0f} km/h, "
          f"{zone.max() + 1} braking zones")


if __name__ == "__main__":
    main()
