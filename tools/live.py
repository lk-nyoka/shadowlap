#!/usr/bin/env python3
"""
Shadow Lap live dashboard.

    python3 tools/live.py                                   # runs on this PC
    python3 tools/live.py --qnx root@172.31.1.10            # can also run laps on QNX
    then open http://localhost:8000

From the page you can:
  * pick a lap, a scenario and where to run it (this PC or QNX), then Start/Stop
  * load a video and press play: the lap starts by itself and the video stays
    in step with the charts
  * calibrate a lap from the video: hold B (or the Brake button) whenever the
    car brakes; the lap is rebuilt so its braking matches the video exactly

It also follows runs started elsewhere (newest out*/<scenario>/compare.csv).
Standard library only.

Private testing only for third-party videos: YouTube's own player streams
them, nothing is downloaded. Don't use third-party footage in the hackathon
video or commit links to it.
"""
import argparse
import glob
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import lapgen  # noqa: E402

FIELDS = ["seq", "t_lap", "zone", "v_kmh", "soc", "lap_harvest_mj", "v1_regen", "v1_applied",
          "v1_latency_us", "v2_present", "v2_regen", "class", "f_rear_grip", "plant_lag_us",
          "v1_friction", "v2_friction", "f_rear_req", "a_req", "brake", "motor_temp_c", "wing_closed",
          "v2_extra_kmh", "v1_class"]
SCENARIOS = ["shadow", "crash", "freeze", "sigstop", "flood", "attack", "baseline"]
CANDIDATES = ["v2", "v2.1", "v3"]
# Track conditions for a live lap. Fixed list, so nothing typed in the browser reaches a shell.
CONDITIONS = {
    "normal": ("Normal day: full grip, 30 °C, battery 80%", "--grip 1.0 --ambient 30 --soc0 0.80"),
    "worn":   ("Worn tyres: 90% grip, 30 °C", "--grip 0.90 --ambient 30 --soc0 0.80"),
    "hot":    ("Hot day: 42 °C, battery 88%", "--grip 1.0 --ambient 42 --soc0 0.88"),
    "tough":  ("Hot day + worn tyres: 90% grip, 42 °C, battery 88%", "--grip 0.90 --ambient 42 --soc0 0.88"),
}
START_DELAY_MS = 1500


def lap_info(path):
    """Duration and metadata for a lap file."""
    dur, last = 0.0, ""
    try:
        with open(path, "rb") as fp:
            fp.seek(max(0, os.path.getsize(path) - 200))
            last = fp.read().decode(errors="ignore").strip().splitlines()[-1]
        dur = float(last.split(",")[0])
    except (OSError, ValueError, IndexError):
        pass
    meta = {}
    mp = path[:-4] + ".json"
    if os.path.exists(mp):
        try:
            meta = json.load(open(mp))
        except ValueError:
            meta = {}
    name = os.path.basename(path)[:-4]
    label = meta.get("label") or ("Track A (synthetic)" if name == "lap_a" else name)
    return {"name": name, "label": label, "duration": round(dur, 2), **meta}


def lap_geometry(name):
    """Track map for a lap: real x,y when the lap has them, else a schematic loop by distance."""
    path = os.path.join(ROOT, "data", re.sub(r"[^A-Za-z0-9_.-]", "", name) + ".csv")
    if not os.path.exists(path):
        return None
    t, d, x, y, br = [], [], [], [], []
    with open(path) as fp:
        head = fp.readline().strip().split(",")
        has_xy = "x" in head and "y" in head
        ix = head.index("x") if has_xy else -1
        iy = head.index("y") if has_xy else -1
        for i, line in enumerate(fp):
            if i % 5:
                continue                     # 20 points a second is plenty for a map
            v = line.strip().split(",")
            if len(v) < 6:
                continue
            t.append(float(v[0]))
            d.append(float(v[1]))
            br.append(int(v[4]))
            if has_xy:
                x.append(float(v[ix]))
                y.append(float(v[iy]))
    if not t:
        return None
    if not has_xy:                           # schematic: a rounded loop, position by distance
        import math
        total = d[-1] or 1.0
        for di in d:
            a = 2 * math.pi * di / total
            x.append(1000 * math.cos(a) * (1.4 + 0.25 * math.cos(3 * a)))
            y.append(600 * math.sin(a))
    return {"name": name, "real": has_xy, "t": t, "x": x, "y": y, "brake": br}


def list_laps():
    return [lap_info(p) for p in sorted(glob.glob(os.path.join(ROOT, "data", "*.csv")))]


class Runner:
    """Starts and stops runs, locally or on QNX over ssh."""

    def __init__(self, outdir, qnx, qnx_dir):
        self.outdir = outdir
        self.qnx = qnx
        self.qnx_dir = qnx_dir
        self.lock = threading.Lock()
        self.proc = None
        self.follower = None
        self.gen = 0
        self.state = {"running": False, "target": None, "scenario": None, "lap": None,
                      "lap_duration": None, "message": "idle", "started": None}
        self.watch = None

    def _log(self, name):
        os.makedirs(os.path.join(ROOT, self.outdir), exist_ok=True)
        return open(os.path.join(ROOT, self.outdir, name), "w")

    def start(self, scenario, target, lap, candidate="v2", cond="normal"):
        if scenario not in SCENARIOS:
            return False, "unknown scenario"
        if candidate not in CANDIDATES:
            return False, "unknown candidate"
        if cond not in CONDITIONS:
            return False, "unknown conditions"
        plant_args = CONDITIONS[cond][1]
        lap_path = os.path.join(ROOT, "data", lap + ".csv")
        if not os.path.exists(lap_path):
            return False, "lap not found"
        if target == "qnx" and not self.qnx:
            return False, "QNX not configured: start the dashboard with --qnx root@<ip>"
        self.stop()
        with self.lock:
            self.gen += 1
            info = lap_info(lap_path)
            if target == "local":
                out = os.path.join(ROOT, self.outdir)
                env = dict(os.environ, LAP=lap_path, START_DELAY_MS=str(START_DELAY_MS), CANDIDATE=candidate,
                           PLANT_ARGS=plant_args)
                self.watch = os.path.join(out, scenario, "compare.csv")
                self._clear(self.watch)
                self.proc = subprocess.Popen(
                    ["sh", os.path.join(ROOT, "scripts", "run_demo.sh"), scenario, out],
                    cwd=ROOT, env=env, stdout=self._log("dashboard-run.log"),
                    stderr=subprocess.STDOUT, start_new_session=True)
            else:
                user_host = self.qnx
                user, host = user_host.split("@", 1) if "@" in user_host else ("root", user_host)
                rdir = self.qnx_dir
                local_dir = os.path.join(ROOT, "out-qnx", scenario)
                os.makedirs(local_dir, exist_ok=True)
                self.watch = os.path.join(local_dir, "compare.csv")
                self._clear(self.watch)
                cp = subprocess.run(["scp", "-q", "-o", "BatchMode=yes", lap_path,
                                     f"{user_host}:{rdir}/data/lap_live.csv"],
                                    capture_output=True, text=True, timeout=30)
                if cp.returncode != 0:
                    return False, "could not copy the lap to QNX: " + (cp.stderr.strip() or "scp failed")
                self.follower = subprocess.Popen(
                    ["sh", os.path.join(TOOLS, "follow_qnx.sh"), host, scenario, rdir, user],
                    cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    start_new_session=True)
                time.sleep(0.8)                     # let the follower start watching
                remote = (f"cd {shlex.quote(rdir)} && LAP=data/lap_live.csv START_DELAY_MS={START_DELAY_MS} CANDIDATE={candidate} "
                          f"PLANT_ARGS={shlex.quote(plant_args)} "
                          f"V2_USER=qnxuser PROD_CPU=0 SHADOW_CPU=1 sh run_demo.sh {scenario} out")
                self.proc = subprocess.Popen(
                    ["ssh", "-tt", "-o", "BatchMode=yes", user_host, remote],
                    stdin=subprocess.DEVNULL, stdout=self._log("dashboard-run-qnx.log"),
                    stderr=subprocess.STDOUT, start_new_session=True)
            self.state = {"running": True, "target": target, "scenario": scenario, "lap": lap, "candidate": candidate, "cond": cond,
                          "lap_duration": info["duration"], "message": "starting", "started": time.time()}
            threading.Thread(target=self._wait, args=(self.proc, self.gen), daemon=True).start()
        return True, "started"

    @staticmethod
    def _clear(path):
        try:
            os.remove(path)
        except OSError:
            pass

    def _wait(self, proc, gen):
        rc = proc.wait()
        time.sleep(2.0)                             # let the last rows arrive
        with self.lock:
            if gen != self.gen:
                return
            if self.follower:
                self._kill(self.follower)
                self.follower = None
            self.state = dict(self.state, running=False,
                              message="finished" if rc == 0 else f"ended (code {rc})")

    @staticmethod
    def _kill(p):
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass

    def stop(self):
        with self.lock:
            was = self.state.get("running")
            target = self.state.get("target")
            if self.proc and self.proc.poll() is None:
                self._kill(self.proc)
            if self.follower:
                self._kill(self.follower)
                self.follower = None
            self.gen += 1
            if was:
                self.state = dict(self.state, running=False, message="stopped")
        if was and target == "qnx":
            subprocess.run(["ssh", "-o", "BatchMode=yes", self.qnx,
                            f"cd {shlex.quote(self.qnx_dir)} && sh run_demo.sh stop out"],
                           capture_output=True, timeout=20)
        elif was:
            subprocess.run(["sh", os.path.join(ROOT, "scripts", "run_demo.sh"), "stop",
                            os.path.join(ROOT, self.outdir)], capture_output=True, timeout=20)


def newest_csv(outdirs):
    files = []
    for d in outdirs:
        files += glob.glob(os.path.join(ROOT, d, "*", "compare.csv"))
    return max(files, key=os.path.getmtime) if files else None


def rows_from(fp, header):
    out = []
    while True:
        pos = fp.tell()
        line = fp.readline()
        if not line:
            break
        if not line.endswith("\n"):          # half-written line: try again later
            fp.seek(pos)
            break
        vals = line.rstrip("\n").split(",")
        if len(vals) != len(header):
            continue
        d = dict(zip(header, vals))
        r = []
        for f in FIELDS:
            v = d.get(f, "")
            try:
                r.append(v if f == "class" else float(v or 0))
            except ValueError:
                r.append(0.0)
        out.append(r)
    return out


class Handler(BaseHTTPRequestHandler):
    runner = None
    outdirs = ["out", "out-qnx"]

    def log_message(self, *a):
        pass

    # ---------- plumbing ----------
    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return {}

    def do_GET(self):
        if self.path.startswith("/events"):
            return self.events()
        if self.path.startswith("/api/laps"):
            return self._json({"laps": list_laps(), "scenarios": SCENARIOS,
                               "qnx": self.runner.qnx})
        if self.path.startswith("/api/lapgeo"):
            from urllib.parse import urlparse, parse_qs
            q = parse_qs(urlparse(self.path).query)
            g = lap_geometry((q.get("lap") or ["lap_a"])[0])
            return self._json(g or {"error": "no such lap"}, 200 if g else 404)
        if self.path.startswith("/api/status"):
            return self._json(self.runner.state)
        body = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        b = self._body()
        if self.path.startswith("/api/start"):
            ok, msg = self.runner.start(b.get("scenario", "shadow"), b.get("target", "local"),
                                        b.get("lap", "lap_a"), b.get("candidate", "v2"),
                                        b.get("cond", "normal"))
            return self._json({"ok": ok, "message": msg, "state": self.runner.state}, 200 if ok else 400)
        if self.path.startswith("/api/stop"):
            self.runner.stop()
            return self._json({"ok": True, "state": self.runner.state})
        if self.path.startswith("/api/calibrate"):
            return self.calibrate(b)
        self._json({"error": "not found"}, 404)

    def calibrate(self, b):
        try:
            duration = float(b["duration"])
            brakes = [(float(x), float(y)) for x, y in b.get("brakes", [])]
        except (KeyError, TypeError, ValueError):
            return self._json({"ok": False, "message": "bad calibration data"}, 400)
        if duration < 5 or not brakes:
            return self._json({"ok": False, "message": "mark at least one braking zone over at least 5 s"}, 400)
        vid = re.sub(r"[^A-Za-z0-9_-]", "", str(b.get("video", "video")))[:40] or "video"
        name = f"lap_video_{vid}"
        rows = lapgen.build(duration, brakes, float(b.get("v_start", 250)), float(b.get("v_max", 320)),
                            float(b.get("v_min", 70)))
        lapgen.write(rows, os.path.join(ROOT, "data", name + ".csv"))
        meta = {"label": f"From video {vid} ({len(brakes)} braking zones)", "video": b.get("video_url", vid),
                "offset": float(b.get("offset", 0)), "brakes": brakes, "duration_marked": duration}
        json.dump(meta, open(os.path.join(ROOT, "data", name + ".json"), "w"), indent=1)
        return self._json({"ok": True, "lap": name, "laps": list_laps()})

    # ---------- live stream ----------
    def send_event(self, kind, data):
        self.wfile.write(f"event: {kind}\ndata: {json.dumps(data)}\n\n".encode())
        self.wfile.flush()

    def events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        cur, fp, header, ino, gen = None, None, None, None, None
        last_status = 0.0
        try:
            while True:
                r = self.runner
                if time.time() - last_status > 0.5:
                    self.send_event("status", r.state)
                    last_status = time.time()
                path = r.watch if (r.watch and r.state.get("running")) else newest_csv(self.outdirs)
                if r.gen != gen:                          # a new run was started or stopped
                    gen, cur = r.gen, None
                    if fp:
                        fp.close()
                        fp = None
                    if r.state.get("running"):            # clear the screen straight away
                        self.send_event("reset", {"scenario": r.state.get("scenario"),
                                                  "source": "qnx" if r.state.get("target") == "qnx" else "local"})
                if path and path == cur and fp:
                    try:
                        st = os.stat(path)
                        if st.st_ino != ino or st.st_size < fp.tell():
                            cur = None
                    except FileNotFoundError:
                        cur = None
                if path and path != cur and os.path.exists(path):
                    if fp:
                        fp.close()
                    fp = open(path)
                    ino = os.fstat(fp.fileno()).st_ino
                    header = fp.readline().rstrip("\n").split(",")
                    if len(header) < 5:
                        fp.close()
                        fp, cur = None, None
                        time.sleep(0.1)
                        continue
                    cur = path
                    self.send_event("reset", {"scenario": os.path.basename(os.path.dirname(path)),
                                              "source": "qnx" if "out-qnx" in path else "local"})
                if fp and cur:
                    rows = rows_from(fp, header)
                    for i in range(0, len(rows), 500):
                        self.send_event("rows", rows[i:i + 500])
                time.sleep(0.05)
        except (BrokenPipeError, ConnectionResetError):
            pass


PAGE = r"""<!doctype html><html><head><meta charset="utf-8"><title>Shadow Lap - live</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#090c11;--panel:#10151c;--panel2:#0c1016;--line:#1d2530;--txt:#e8edf3;--mut:#8592a3;--v1:#3ecf8e;--v2:#ff8a3d;--warn:#f5b13d;--bad:#ff5a5a;--cap:#9b8cff;--blue:#5aa9ff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--txt);font:14px/1.5 Inter,system-ui,-apple-system,Segoe UI,sans-serif}
header{display:flex;align-items:center;gap:16px;padding:16px 24px;border-bottom:1px solid var(--line)}
.logo{width:34px;height:34px;border-radius:9px;background:linear-gradient(135deg,#1c2735,#0e141c);border:1px solid var(--line);display:grid;place-items:center}
h1{font-size:19px;font-weight:650;letter-spacing:-.01em;margin:0}.sub{font-size:12px;color:var(--mut)}
.scn{margin-left:auto;font:12px ui-monospace,Menlo,monospace;color:var(--mut);border:1px solid var(--line);padding:5px 12px;border-radius:99px;background:var(--panel2)}
.live{width:8px;height:8px;border-radius:50%;background:var(--mut);display:inline-block;margin-right:8px}
.live.on{background:var(--v1);box-shadow:0 0 8px var(--v1);animation:p 1.2s infinite}@keyframes p{50%{opacity:.35}}
select,input{background:var(--panel2);border:1px solid var(--line);color:var(--txt);border-radius:8px;padding:8px 10px;font-size:13px;width:100%}
label.f{display:block;font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.06em;margin:10px 0 4px}
button{background:var(--txt);color:#090c11;border:0;border-radius:8px;padding:9px 14px;font-size:13px;font-weight:600;cursor:pointer}
button.go{background:#2bb673;color:#04140c;width:100%;margin-top:14px;padding:11px}button.ghost{background:transparent;color:var(--txt);border:1px solid var(--line)}
button:disabled{opacity:.4;cursor:default}button.brake{background:var(--bad);color:#fff}button.brake.held{background:#fff;color:var(--bad)}
.wrap{display:grid;grid-template-columns:270px minmax(0,1.15fr) minmax(0,1fr);gap:14px;padding:14px 24px}
.col{display:flex;flex-direction:column;gap:14px;min-width:0}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:14px 16px}
.card h2{font-size:12px;font-weight:600;color:var(--mut);margin:0 0 10px;text-transform:uppercase;letter-spacing:.07em}
.q{font-size:11px;color:var(--blue);text-transform:uppercase;letter-spacing:.08em;margin:2px 0 -6px 2px;font-weight:600}
canvas{width:100%;display:block}
.tiles{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}
.tile{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:12px}
.tile span{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.07em;display:block}
.tile b{font-size:21px;font-weight:650;font-variant-numeric:tabular-nums;letter-spacing:-.02em}
.tiles3{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.tiles3 .tile{background:var(--panel2);padding:10px}.tiles3 .tile b{font-size:18px}
.ok{color:var(--v1)}.bad{color:var(--bad)}.warn{color:var(--warn)}
.legend{display:flex;gap:12px;font-size:11px;color:var(--mut);margin-top:6px;flex-wrap:wrap}.legend i{display:inline-block;width:10px;height:3px;margin-right:6px;vertical-align:middle}
.verdict{border-radius:12px;padding:14px;text-align:center;border:1px solid var(--line);background:var(--panel2)}
.verdict>b{display:block;font-size:20px;letter-spacing:.02em;margin:2px 0}.verdict p b{color:var(--txt)}.verdict p{margin:4px 0 0;font-size:12px;color:var(--mut)}
.verdict.fail{border-color:rgba(255,90,90,.6);background:rgba(255,90,90,.07)}.verdict.fail b{color:var(--bad)}
.verdict.clean{border-color:rgba(62,207,142,.5);background:rgba(62,207,142,.06)}.verdict.clean b{color:var(--v1)}
table.chk{width:100%;border-collapse:collapse;font-size:13px}table.chk th{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.06em;text-align:left;font-weight:600;padding:4px 6px;border-bottom:1px solid var(--line)}
table.chk td{padding:7px 6px;border-bottom:1px solid var(--line);font-variant-numeric:tabular-nums}table.chk td.k{color:var(--mut);font-size:11px}
.pill{font-size:11px;padding:2px 8px;border-radius:99px;font-weight:600}.pill.ok{background:rgba(62,207,142,.12)}.pill.bad{background:rgba(255,90,90,.14)}.pill.warn{background:rgba(245,177,61,.14)}.pill.mut{color:var(--mut);background:#151b23}
#log{list-style:none;margin:0;padding:0;max-height:300px;overflow:auto;font-size:13px}
#log li{padding:9px 0;border-bottom:1px solid var(--line)}#log li em{font-style:normal;font:11px ui-monospace,Menlo,monospace;color:var(--mut);display:block}
.tag{font-size:10px;padding:2px 7px;border-radius:99px;margin-right:6px;color:#090c11;font-weight:700;text-transform:uppercase;letter-spacing:.04em}
.lower{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;padding:0 24px 14px}
.lower2{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:14px;padding:0 24px 14px}
.vwrap{position:relative}.vid{aspect-ratio:16/9;background:#000;border-radius:10px;overflow:hidden;display:flex;align-items:center;justify-content:center;color:var(--mut);font-size:12px}
.vid iframe,.vid video{width:100%;height:100%;border:0}
.over{position:absolute;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.55);border-radius:10px;font-size:15px;font-weight:600}
.vrow{display:flex;gap:6px;margin-top:8px;align-items:center;flex-wrap:wrap}.vrow input#yt{flex:1;min-width:0;width:auto}.vrow input#off{width:70px}.vrow input[type=checkbox]{width:auto}
.vrow label{font-size:12px;color:var(--mut);cursor:pointer}.vrow input[type=file]{width:auto}
.mono{font:11px ui-monospace,Menlo,monospace;color:var(--mut);margin-top:6px}
#calib{display:none;margin-top:10px;border-top:1px solid var(--line);padding-top:10px}
.foot{color:var(--mut);font-size:12px;padding:4px 24px 24px}
.story{font-size:14px;line-height:1.5;min-height:44px;margin:0 0 10px}
.split{display:grid;grid-template-columns:120px 1fr 122px;gap:10px;align-items:center;margin:7px 0}
.split .lbl{font-size:12px;color:var(--mut)}.split .lbl b{color:var(--txt)}.split .val{font:11px ui-monospace,Menlo,monospace;text-align:right;white-space:nowrap}
.bar{position:relative;height:20px;background:var(--panel2);border:1px solid var(--line);border-radius:6px;overflow:hidden}
.bar i{position:absolute;top:0;bottom:0}.bar .lim{width:2px;background:#fff;opacity:.9}.bar.risk{border-color:var(--bad);box-shadow:0 0 0 1px var(--bad)}
.mini{display:flex;gap:14px;flex-wrap:wrap;font-size:12px;color:var(--mut);margin-top:8px}.mini b{color:var(--txt);font-weight:600}
@media (max-width:1150px){.wrap{grid-template-columns:1fr}.lower,.lower2{grid-template-columns:1fr}.tiles{grid-template-columns:repeat(2,1fr)}}
</style></head><body>
<header><div class="logo"><svg width="18" height="18" viewBox="0 0 18 18"><path d="M2 13c3-8 9-8 14-6" stroke="#5aa9ff" stroke-width="2" fill="none" stroke-linecap="round"/><path d="M2 15c3-6 8-6 12-4" stroke="#ff8a3d" stroke-width="1.6" fill="none" stroke-dasharray="2 2" stroke-linecap="round"/></svg></div>
 <div><h1>Shadow Lap</h1><div class="sub">Test the next brake controller without putting it in control</div></div>
 <span class="scn" id="scn"><span class="live" id="dot"></span><span id="scnt">waiting for a run</span></span></header>
<div class="wrap">
 <div class="col">
  <div class="card"><h2>Test configuration</h2>
   <label class="f">Lap</label><select id="lap"></select>
   <label class="f">Run on</label><select id="target"><option value="local">This PC</option><option value="qnx">QNX</option></select>
   <label class="f">Scenario</label><select id="scen"></select>
   <label class="f">Conditions</label><select id="cond"><option value="normal">Normal day</option><option value="worn">Worn tyres (90% grip)</option><option value="hot">Hot day (42 °C)</option><option value="tough">Hot day + worn tyres</option></select>
   <label class="f">Candidate</label><select id="cand"><option value="v2">v2 (more rear braking, more harvest)</option><option value="v2.1">v2.1 (v2 with its flaws fixed)</option><option value="v3">v3 (smoother blending)</option></select>
   <button id="start" class="go">▶ Start lap</button><button id="stop" class="ghost" style="width:100%;margin-top:8px" disabled>Stop</button>
  </div>
  <div class="card"><h2>This lap</h2>
   <div class="verdict" id="verdict"><b id="vd_t">Waiting</b><p id="vd_p">Start a lap to see whether the candidate breaks a limit.</p></div>
   <p class="mono" style="margin-top:10px">One lap is a demonstration. The release verdict (PASS_WITHIN_SCOPE / FAIL / INVALID) comes from the 600-lap sweep and <code>tools/gate.py</code>.</p>
  </div>
 </div>
 <div class="col">
  <div class="card"><h2 id="maph">The car on track</h2><canvas id="map" height="330"></canvas>
   <div class="legend"><span><i style="background:#5aa9ff"></i>track</span><span><i style="background:#ff8a3d"></i>braking zones</span><span><i style="background:#3ecf8e;height:8px;width:8px;border-radius:50%"></i>the car now</span><span>dots: where the candidate broke a limit</span></div>
   <div class="legend" style="margin-top:2px"><span>Lap data: OpenF1 (independent, CC BY-NC-SA 4.0, non-commercial). Speeds and positions are real; forces, battery and motor are modelled; extra speed is an estimate.</span></div></div>
  <div class="q">1 · What is the candidate doing?</div>
  <div class="card"><h2>Limit checks, this lap</h2>
   <table class="chk"><thead><tr><th>Check</th><th></th><th>v1 (in charge)</th><th>Candidate</th><th>Candidate's closest approach</th></tr></thead><tbody id="chk"></tbody></table>
   <div class="legend"><span>Hard limits fail on one 10 ms frame; soft limits once they last 50 ms. Closest approach: share of the limit (1.00 = at it), computed for display.</span></div></div>
 </div>
 <div class="col">
  <div class="tiles">
   <div class="tile"><span>Speed</span><b id="s_v">-</b> <span style="display:inline">km/h</span></div>
   <div class="tile"><span>Battery</span><b id="s_soc">-</b></div>
   <div class="tile"><span>Motor</span><b id="s_mot">-</b></div>
   <div class="tile"><span>Lap time</span><b id="s_t">-</b></div>
  </div>
  <div class="card"><h2>Rear brakes right now</h2>
   <div id="story" class="story">Press Start lap, or play a video, to begin.</div>
   <div class="split"><div class="lbl">Driver asks the rear for</div><div class="val" id="b_req" style="text-align:left">-</div><div></div></div>
   <div class="split"><div class="lbl"><b>v1</b> in charge</div><div class="bar" id="bar1"></div><div class="val" id="b_v1">-</div></div>
   <div class="split"><div class="lbl"><b>Candidate</b> in shadow</div><div class="bar" id="bar2"></div><div class="val" id="b_v2">-</div></div>
   <div class="legend"><span><i style="background:var(--v1)"></i>motor (charges the battery)</span><span><i style="background:#6b7583"></i>brake discs</span><span><i style="background:#fff;width:2px;height:12px"></i>tyre grip limit</span></div>
   <div class="mini"><span>Wings <b id="m_wing">-</b></span><span>Braking <b id="m_g">-</b></span><span>Candidate <b id="s_v2">-</b></span><span style="display:none"><b id="m_soc"></b><b id="m_mot"></b></span></div>
  </div>
  <div class="q">2 · Is the trusted controller unaffected?</div>
  <div class="card"><div class="tiles3">
   <div class="tile"><span>v1 deadline misses</span><b id="s_miss" class="ok">0</b></div>
   <div class="tile"><span>v1 response now</span><b id="s_lat">-</b></div>
   <div class="tile"><span>v1 slowest so far</span><b id="s_worst">-</b></div>
   <div class="tile"><span>Margin to 8 ms deadline</span><b id="s_marg">-</b></div>
  </div></div>
  <div class="q">3 · Can we trust this evidence?</div>
  <div class="card"><div class="tiles3">
   <div class="tile"><span>Frames checked</span><b id="s_frames">0</b></div>
   <div class="tile"><span>Candidate answers missing</span><b id="s_gap" class="ok">0</b></div>
   <div class="tile" title="Frames where the simulated car itself woke more than 2 ms late for its 10 ms tick, because the computer or VM paused it. v1 is timed from when each frame was actually sent, so these are not v1 misses."><span>Car sim woke late (&gt;2 ms, host)</span><b id="s_ovr" class="ok">0</b></div>
   <div class="tile"><span>Running on</span><b id="s_on" style="font-size:15px">-</b></div>
  </div></div>
 </div>
</div>
<div class="lower">
 <div class="card"><h2>Speed, coloured where the candidate broke a limit</h2><canvas id="c1" height="180"></canvas>
  <div class="legend"><span><i style="background:#ff5a5a"></i>rear lock</span><span><i style="background:#f5b13d"></i>battery</span><span><i style="background:#9b8cff"></i>lap energy</span><span><i style="background:#e05fa0"></i>hot motor</span><span><i style="background:#5aa9ff"></i>demand not met</span><span><i style="background:#3d5a7a"></i>different, within limits</span><span><i style="background:#555"></i>no answer</span></div></div>
 <div class="card"><h2>Motor harvesting, kW</h2><canvas id="c2" height="180"></canvas>
  <div class="legend"><span><i style="background:var(--v1)"></i>v1 (applied to the car)</span><span><i style="background:var(--v2)"></i>candidate (proposed only)</span></div></div>
 <div class="card"><h2>v1 frame-to-command time (log scale)</h2><canvas id="c3" height="180"></canvas>
  <div class="legend"><span><i style="background:var(--v1)"></i>v1 response</span><span><i style="background:var(--bad)"></i>8 ms deadline</span></div></div>
</div>
<div class="lower2">
 <div class="card"><h2>Recent events</h2><ul id="log"></ul></div>
 <div class="card"><h2>Video (optional, synced to lap time)</h2>
  <div class="vwrap"><div class="vid" id="vid">Paste a YouTube link or choose a file</div><div class="over" id="over"></div></div>
  <div class="vrow"><input id="yt" placeholder="YouTube link"><input id="off" type="number" step="0.1" value="0" title="seconds into the video where the lap starts"><button id="vload">Load</button></div>
  <div class="vrow"><label>or a file: <input id="vfile" type="file" accept="video/*"></label></div>
  <div class="vrow"><label><input id="vauto" type="checkbox" checked> start the lap when I press play</label><label><input id="vsync" type="checkbox" checked> keep in step</label></div>
  <div class="vrow"><button id="cal" class="ghost">Calibrate lap from this video</button></div>
  <div id="calib">
   <div style="font-size:13px;margin-bottom:8px">Watch the lap. <b>Hold B</b> (or hold the red button) every time the car brakes. Click <b>Finish</b> when the lap ends.</div>
   <div class="vrow"><button id="hold" class="brake">Hold while braking</button><button id="fin">Finish</button><button id="calx" class="ghost">Cancel</button></div>
   <div class="mono" id="calst">0 braking zones marked</div>
  </div>
  <div class="mono" id="vstat">offset = seconds into the video where the lap starts</div></div>
</div>
<div class="foot">The candidate's decisions are never applied to the car. Battery, motor and forces are a simulated model; checks are project-defined rules, not certified safety mechanisms.</div>
<script>
const COL={rear_lock_risk:'#ff5a5a',over_battery:'#f5b13d',over_lap_cap:'#9b8cff',over_motor:'#e05fa0',demand_not_met:'#5aa9ff',within_limits:'#3d5a7a',shadow_missing:'#555'};
const TXT={rear_lock_risk:'asked the rear tyres for more grip than they had',over_battery:'tried to charge harder than the nearly full battery accepts',over_lap_cap:'kept harvesting after the lap energy limit',over_motor:'asked a hot motor for more braking than it could safely give',demand_not_met:'delivered less rear braking than the modelled rear demand, despite available rear grip',within_limits:'chose a different split, within limits',shadow_missing:'stopped answering (crashed, frozen or behind)'};
const $=id=>document.getElementById(id);
let R=[],misses=0,ovr=0,seen=new Set(),lastCls='agree',dirty=true,lastRowWall=0,lapDur=90,laps=[],run={running:false};
/* ---------- limit checks: running tally for this lap ---------- */
const CHK=[['rear_lock_risk','Rear grip','hard','grip'],['demand_not_met','Braking demand','hard','demand'],
 ['over_battery','Battery charge','soft','battery'],['over_motor','Motor heat','soft','motor'],['over_lap_cap','Lap energy','soft','lap']];
let A;function aggReset(){A={frames:0,missing:0,worstLat:0,c:{},v:{},ep:{},run:{},prev:null,first:null,app:{grip:0,demand:9,battery:0,motor:0,lap:0}};
 for(const [k] of CHK){A.c[k]=0;A.v[k]=0;A.ep[k]=0;A.run[k]=0}}aggReset();
function accFrac(soc){return soc<=.8?1:soc>=.98?0:(.98-soc)/.18}
function motLim(vms,temp){let l=9000;if(vms>1&&350e3/vms<l)l=350e3/vms;if(temp>=160)return 0;if(temp>130)l*=(160-temp)/30;return l}
function approach(o){if(!o.brk||!o.p2)return;const ap=A.app,rear=o.r2+o.f2,vms=o.v/3.6,p=o.r2*vms;
 if(o.grip>0)ap.grip=Math.max(ap.grip,rear/o.grip);
 const want=Math.min(o.req,.95*o.grip);if(want>500)ap.demand=Math.min(ap.demand,rear/want);
 const pb=350e3*accFrac(o.soc);if(pb>1)ap.battery=Math.max(ap.battery,p/pb);
 const ml=motLim(vms,o.mot);if(ml>1)ap.motor=Math.max(ap.motor,o.r2/ml);
 if(p>1)ap.lap=Math.max(ap.lap,(o.mj*1e6+p*.01)/2.4e6)}
function agg(o){A.frames++;if(!o.p2)A.missing++;if(o.lat>A.worstLat)A.worstLat=o.lat;
 for(const [k,,kind] of CHK){if(o.cls===k){A.c[k]++;A.run[k]++;const need=kind==='hard'?1:5;
   if(A.run[k]===need){A.ep[k]++;if(!A.first)A.first={k,t:o.t,v:o.v}}}else A.run[k]=0;
  if(o.v1c===k)A.v[k]++}
 approach(o)}
/* ---------- data ---------- */
function reset(s){R=[];misses=0;ovr=0;seen=new Set();lastCls='agree';lastRowWall=0;$('log').innerHTML='';aggReset();
 $('s_on').textContent=s.source==='qnx'?'QNX':'this PC';
 $('scnt').textContent=s.scenario+(s.source==='qnx'?' · QNX':' · this PC')+(run&&run.candidate&&run.running?' · '+run.candidate:'');dirty=true;if(ready)setTimeout(()=>sync(true),200)}
function add(rows){if(rows.length)lastRowWall=Date.now();for(const r of rows){
  const [seq,t,zone,v,soc,mj,r1,app,lat,p2,r2,cls,grip,lag,f1,f2,req,areq,brk,mot,wing,xk,v1c]=r;if(lag>2000)ovr++;
  const o={seq,t,zone,v,soc,mj,k1:r1*v/3.6/1e3,app,lat,p2,k2:p2?r2*v/3.6/1e3:null,cls,grip,r1,r2,f1,f2,req,areq,brk,mot,wing,xk:xk||0,v1c:v1c||'agree'};R.push(o);if(!app)misses++;agg(o);
  const key=cls+'@'+zone;
  if(cls!==lastCls&&cls!=='agree'&&cls!=='within_limits'&&!(zone>=0&&seen.has(key))){seen.add(key);const li=document.createElement('li');
   li.innerHTML=`<span class="tag" style="background:${COL[cls]}">${cls.replace(/_/g,' ')}</span>Candidate ${TXT[cls]}<em>${t.toFixed(1)} s · ${zone>=0?'braking zone '+(zone+1):'on throttle'} · ${v.toFixed(0)} km/h · battery ${(soc*100).toFixed(0)}%</em>`;
   $('log').prepend(li)}
  lastCls=cls}dirty=true}
/* ---------- drawing ---------- */
function setup(c){const d=devicePixelRatio||1,w=c.clientWidth,h=c.height;if(c.width!==w*d){c.width=w*d;c.style.height=h+'px';c._h=h}const g=c.getContext('2d');g.setTransform(d,0,0,d,0,0);g.clearRect(0,0,w,c._h||h);g.shadowBlur=0;return [g,w,c._h||h]}
function axes(g,w,h,ymin,ymax,ticks,fmt){g.strokeStyle='#1d2530';g.fillStyle='#8592a3';g.font='11px system-ui';g.lineWidth=1;
 for(const y of ticks){const py=h-18-(y-ymin)/(ymax-ymin)*(h-26);g.beginPath();g.moveTo(36,py);g.lineTo(w,py);g.stroke();g.fillText(fmt(y),0,py+4)}}
function X(t,w,tmax){return 36+t/tmax*(w-40)}
function cursor(g,w,h,tmax){const t=lapT();if(t===null)return;g.strokeStyle='rgba(236,236,234,.35)';g.lineWidth=1;g.beginPath();const x=X(t,w,tmax);g.moveTo(x,4);g.lineTo(x,h-18);g.stroke()}
function draw(){requestAnimationFrame(draw);if(!R.length){return}
 const last=R[R.length-1],tmax=Math.max(lapDur||0,last.t+1,10);
 $('s_t').textContent=last.t.toFixed(1)+' s';$('s_v').textContent=last.v.toFixed(0);$('s_soc').textContent=(last.soc*100).toFixed(0)+'%';
 $('s_miss').textContent=misses;$('s_miss').className=misses?'bad':'ok';$('s_ovr').textContent=ovr;$('s_ovr').className=ovr?'warn':'ok';
 $('s_lat').textContent=last.lat>=0?(last.lat<1000?last.lat.toFixed(0)+' µs':(last.lat/1000).toFixed(1)+' ms'):'missed';
 const v2=$('s_v2');if(!last.p2){v2.textContent='not answering';v2.className='warn'}else if(last.cls==='agree'){v2.textContent='agrees';v2.className='ok'}else if(last.cls==='within_limits'){v2.textContent='differs, within limits';v2.className=''}else{v2.textContent='breaking a limit';v2.className='bad'}
 $('s_mot').textContent=last.mot.toFixed(0)+' °C';$('s_frames').textContent=A.frames.toLocaleString();
 const gap=$('s_gap');gap.textContent=A.missing;gap.className=A.missing?'warn':'ok';
 const ms=v=>v<1000?v.toFixed(0)+' µs':(v/1000).toFixed(2)+' ms';
 $('s_worst').textContent=A.worstLat>0?ms(A.worstLat):'-';const mg=$('s_marg');
 if(misses){mg.textContent='missed';mg.className='bad'}else if(A.worstLat>0){mg.textContent=((8000-A.worstLat)/1000).toFixed(2)+' ms';mg.className='ok'}
 let [g,w,h]=setup($('c1'));axes(g,w,h,0,340,[0,100,200,300],y=>y);
 g.lineWidth=1.5;g.strokeStyle='#c9d4e2';g.beginPath();R.forEach((o,i)=>{const x=X(o.t,w,tmax),y=h-18-o.v/340*(h-26);i?g.lineTo(x,y):g.moveTo(x,y)});g.stroke();
 for(const o of R){if(o.cls!=='agree'){g.fillStyle=COL[o.cls];g.fillRect(X(o.t,w,tmax)-1,h-18-o.v/340*(h-26)-2,3,4)}}
 cursor(g,w,h,tmax);
 [g,w,h]=setup($('c2'));axes(g,w,h,0,360,[0,100,200,300],y=>y);
 for(const [k,c] of [['k1','#3ecf8e'],['k2','#ff6b3d']]){g.strokeStyle=c;g.lineWidth=k==='k1'?1.8:1.2;g.setLineDash(k==='k2'?[4,3]:[]);g.beginPath();let pen=false;
  for(const o of R){if(o[k]===null){pen=false;continue}const x=X(o.t,w,tmax),y=h-18-o[k]/360*(h-26);pen?g.lineTo(x,y):g.moveTo(x,y);pen=true}g.stroke()}g.setLineDash([]);
 cursor(g,w,h,tmax);
 [g,w,h]=setup($('c3'));const L=v=>Math.log10(Math.max(v,10));axes(g,w,h,1,4.3,[1,2,3,4],y=>({1:'10µs',2:'100µs',3:'1ms',4:'10ms'})[y]);
 const dy=h-18-(L(8000)-1)/3.3*(h-26);g.strokeStyle='#ff4d4d';g.setLineDash([5,4]);g.beginPath();g.moveTo(36,dy);g.lineTo(w,dy);g.stroke();g.setLineDash([]);
 g.fillStyle='#3ecf8e';for(const o of R){const x=X(o.t,w,tmax);if(o.lat<0){g.fillStyle='#ff4d4d';g.fillRect(x-1,dy-6,3,12);g.fillStyle='#3ecf8e';continue}g.fillRect(x,h-18-(L(o.lat)-1)/3.3*(h-26),1.2,1.2)}
}
requestAnimationFrame(draw);addEventListener('resize',()=>{dirty=true});
/* ---------- track map + rear-brakes panel ---------- */
let geo=null,geoName=null;
async function loadGeo(name){if(!name||name===geoName)return;geoName=name;
 try{const r=await fetch('/api/lapgeo?lap='+encodeURIComponent(name));geo=r.ok?await r.json():null}catch(e){geo=null}
 $('maph').textContent=geo?(geo.real?'The car on track (real position data, ~4 samples/s, interpolated)':'The car on track (schematic: this lap has no position data)'):'The car on track'}
function geoAt(t){if(!geo)return null;const T=geo.t;let lo=0,hi=T.length-1;if(t<=T[0])return 0;if(t>=T[hi])return hi;
 while(hi-lo>1){const m=(lo+hi)>>1;if(T[m]<=t)lo=m;else hi=m}return lo}
function drawMap(){const c=$('map');let [g,w,h]=setup(c);if(!geo){g.fillStyle='#8b8b90';g.font='13px system-ui';g.fillText('Pick a lap to see the track.',16,30);return}
 const X=geo.x,Y=geo.y;let x0=Math.min(...X),x1=Math.max(...X),y0=Math.min(...Y),y1=Math.max(...Y);
 const pad=18,sc=Math.min((w-2*pad)/(x1-x0||1),(h-2*pad)/(y1-y0||1)),ox=(w-(x1-x0)*sc)/2,oy=(h-(y1-y0)*sc)/2;
 const px=i=>ox+(X[i]-x0)*sc,py=i=>h-(oy+(Y[i]-y0)*sc);
 const path=()=>{g.beginPath();X.forEach((_,i)=>i?g.lineTo(px(i),py(i)):g.moveTo(px(i),py(i)))};
 g.lineCap='round';g.lineJoin='round';g.strokeStyle='#16202c';g.lineWidth=12;path();g.stroke();
 g.shadowColor='rgba(90,169,255,.9)';g.shadowBlur=14;g.strokeStyle='#5aa9ff';g.lineWidth=2.5;path();g.stroke();g.shadowBlur=0;
 g.strokeStyle='#ff8a3d';g.lineWidth=4;for(let i=1;i<X.length;i++){if(geo.brake[i]&&geo.brake[i-1]){g.beginPath();g.moveTo(px(i-1),py(i-1));g.lineTo(px(i),py(i));g.stroke()}}
 g.beginPath();g.fillStyle='#e8edf3';g.arc(px(0),py(0),4,0,7);g.fill();g.font='11px system-ui';g.fillText('start',px(0)+7,py(0)-6);
 for(const o of R){if(o.cls==='agree'||o.cls==='within_limits')continue;const i=geoAt(o.t);if(i===null)continue;g.fillStyle=COL[o.cls];g.fillRect(px(i)-2,py(i)-2,4,4)}
 const t=lapT();if(t!==null){const i=geoAt(t);g.beginPath();g.fillStyle='rgba(62,207,142,.22)';g.arc(px(i),py(i),14,0,7);g.fill();g.beginPath();g.shadowColor='#3ecf8e';g.shadowBlur=12;g.fillStyle='#3ecf8e';g.arc(px(i),py(i),6,0,7);g.fill();g.shadowBlur=0}}
function kN(f){return (f/1000).toFixed(1)+' kN'}
function setBar(id,regen,fric,grip,scale,over){const b=$(id);const W=v=>Math.max(0,Math.min(100,v/scale*100))+'%';
 b.innerHTML=`<i style="left:0;width:${W(regen)};background:var(--v1)"></i><i style="left:${W(regen)};width:${W(fric)};background:#6b7583"></i><i class="lim" style="left:${W(grip)}"></i>`;b.classList.toggle("risk",!!over)}
function story(o){if(!o.brk||o.req<1){return `On the throttle at <b>${o.v.toFixed(0)} km/h</b>. No braking, so nothing for the brake controllers to decide.`}
 const g=(o.areq/9.81).toFixed(1),base=`Braking from <b>${o.v.toFixed(0)} km/h</b> at <b>${g} g</b>. `;
 if(!o.p2)return base+'The candidate is not answering (crashed, frozen or behind). v1 keeps braking normally.';
 const m=o.r1+o.f1>0?Math.round(o.r1/(o.r1+o.f1)*100):0;
 switch(o.cls){case 'agree':return base+`v1 and the candidate agree: ${m}% of the rear braking from the motor, the rest from the discs.`;
  case 'within_limits':return base+'The candidate picks a different split, inside every limit.';
  case 'rear_lock_risk':return base+'<span style="color:var(--bad)">The candidate would ask the rear tyres for more than they can grip: the rear wheels could lock.</span> v1 stays under the limit.';
  case 'over_battery':return base+'<span style="color:var(--warn)">The candidate would charge the nearly full battery harder than it can take.</span> v1 eases off.';
  case 'over_lap_cap':return base+'<span style="color:#9b8cff">The candidate keeps harvesting after the lap\'s energy limit is used up.</span> v1 stops.';
  case 'over_motor':return base+'<span style="color:#e05fa0">The motor is hot; the candidate still asks it for full braking.</span> v1 backs off.';
  case 'demand_not_met':return base+'<span style="color:#5aa9ff">The candidate delivered less rear braking than the modelled rear demand, despite available rear grip: the motor may not charge here and the discs are not told to make up the difference.'+(o.xk>0.5?' Estimated: the car would be <b>'+o.xk.toFixed(1)+' km/h too fast</b> by now.':'')+'</span> v1 delivers the full request.';}
 return base}
function fmtA(k){const a=A.app[k];if(k==='demand')return a>8?'-':a.toFixed(2)+' delivered';return a>0?a.toFixed(2):'-'}
function drawChecks(){const rows=[];for(const [k,name,kind,ak] of CHK){
  const cc=A.c[k],ep=A.ep[k],vv=A.v[k];
  const cand=ep?`<span class="pill bad">${kind==='hard'?cc+' frame'+(cc>1?'s':''):ep+' episode'+(ep>1?'s':'')}</span>`:(cc?`<span class="pill warn">${cc} short (&lt;50 ms)</span>`:(A.frames?'<span class="pill ok">OK</span>':'<span class="pill mut">-</span>'));
  const v1=vv?`<span class="pill bad">${vv} frames</span>`:(A.frames?'<span class="pill ok">OK</span>':'<span class="pill mut">-</span>');
  rows.push(`<tr><td>${name}</td><td class="k">${kind}</td><td>${v1}</td><td>${cand}</td><td>${fmtA(ak)}</td></tr>`)}
 $('chk').innerHTML=rows.join('');
 const vd=$('verdict');
 if(!A.frames){vd.className='verdict';$('vd_t').textContent=run.running?'Starting…':'Waiting';$('vd_p').textContent='Start a lap to see whether the candidate breaks a limit.';return}
 if(A.first){const f=A.first,n=CHK.find(c=>c[0]===f.k);vd.className='verdict fail';$('vd_t').textContent='FAIL (this lap)';
  $('vd_p').innerHTML=`First: <b>${n[1].toLowerCase()}</b> at ${f.t.toFixed(1)} s, ${f.v.toFixed(0)} km/h.`+(A.missing?` ${A.missing} candidate answers missing.`:'');return}
 vd.className='verdict clean';$('vd_t').textContent=run.running?'No limit broken so far':'No limit broken on this lap';
 $('vd_p').textContent=(A.missing?A.missing+' candidate answers missing: evidence has gaps. ':'')+'One lap only; not a release verdict.'}
function drawNow(){drawMap();drawChecks();if(!R.length)return;const t=lapT();let o=R[R.length-1];
 if(t!==null&&o.t>t){for(let i=R.length-1;i>=0;i--){if(R[i].t<=t){o=R[i];break}}}
 $('story').innerHTML=story(o);
 const scale=Math.max(o.req*1.15,o.grip*1.05,o.r2+o.f2,1);
 setBar('bar1',o.r1,o.f1,o.grip,scale,o.r1+o.f1>o.grip*1.001);setBar('bar2',o.p2?o.r2:0,o.p2?o.f2:0,o.grip,scale,o.p2&&o.r2+o.f2>o.grip*1.001);
 $('b_req').textContent=o.brk?kN(o.req):'0 kN';$('b_v1').textContent=kN(o.r1)+' + '+kN(o.f1);$('b_v2').textContent=o.p2?kN(o.r2)+' + '+kN(o.f2):'no answer';
 $('m_soc').textContent=(o.soc*100).toFixed(0)+'%';$('m_mot').textContent=o.mot.toFixed(0)+' °C';
 $('m_wing').textContent=o.wing>=0.99?'corner mode':(o.wing<=0.01?'straight mode':'closing');$('m_g').textContent=o.brk?(o.areq/9.81).toFixed(1)+' g':'-'}
(function loop(){try{drawNow()}catch(e){}setTimeout(()=>requestAnimationFrame(loop),66)})();
/* ---------- run control ---------- */
async function api(path,body){const r=await fetch(path,{method:body?'POST':'GET',headers:{'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined});return r.json()}
async function loadLaps(select){const d=await api('/api/laps');laps=d.laps;
 const cur=select||$('lap').value;$('lap').innerHTML=laps.map(l=>`<option value="${l.name}">${l.label} · ${l.duration.toFixed(1)} s</option>`).join('');
 if(cur&&laps.some(l=>l.name===cur))$('lap').value=cur;
 else{const real=laps.find(l=>l.name.startsWith('lap_openf1_'));if(real)$('lap').value=real.name}
 if(!$('scen').options.length)$('scen').innerHTML=d.scenarios.map(s=>`<option>${s}</option>`).join('');
 const q=$('target').querySelector('option[value=qnx]');q.disabled=!d.qnx;q.textContent=d.qnx?'QNX ('+d.qnx+')':'QNX (start dashboard with --qnx)';
 lapChanged()}
function curLap(){return laps.find(l=>l.name===$('lap').value)}
function lapChanged(){const l=curLap();if(!l)return;lapDur=l.duration;if(!run.running)loadGeo(l.name);
 if(l.video&&!$('yt').value){$('yt').value=l.video}
 if(l.offset!==undefined){$('off').value=l.offset;offset=l.offset}}
$('lap').onchange=lapChanged;
function showOver(msg){const o=$('over');o.textContent=msg||'';o.style.display=msg?'flex':'none'}
async function startRun(){if(cal.on)return;showOver('Starting the lap…');$('start').disabled=true;
 const r=await api('/api/start',{lap:$('lap').value,target:$('target').value,scenario:$('scen').value,candidate:$('cand').value,cond:$('cond').value});
 if(!r.ok){showOver('');$('start').disabled=false;$('vstat').textContent='could not start: '+r.message;return}
 run=r.state;lapDur=run.lap_duration||lapDur;loadGeo(run.lap);setRunUi()}
async function stopRun(){await api('/api/stop',{});showOver('');if(player&&ready)player.pauseVideo&&player.pauseVideo()}
$('start').onclick=()=>{if(ready){vPause();vSeek(offset)}startRun()};$('stop').onclick=stopRun;
function setRunUi(){$('dot').className='live'+(run.running?' on':'');$('start').disabled=run.running||cal.on;$('stop').disabled=!run.running;
 if(!run.running)showOver('')}
/* ---------- events ---------- */
const es=new EventSource('/events');
es.addEventListener('reset',e=>reset(JSON.parse(e.data)));
es.addEventListener('rows',e=>{const rows=JSON.parse(e.data);const first=!R.length;add(rows);if(first&&rows.length){showOver('');if(ready)sync(true)}});
es.addEventListener('status',e=>{const s=JSON.parse(e.data);const was=run.running;run=s;if(s.lap_duration)lapDur=s.lap_duration;if(s.lap)loadGeo(s.lap);setRunUi();
 if(was&&!s.running)$('vstat').textContent='lap '+(s.message||'ended')+(ready?' · the video is free to pause or scrub':'')});
/* ---------- video ---------- */
let player=null,ready=false,offset=0,kind=null,lastProg=0;
function ytId(u){u=(u||'').trim();let m=u.match(/(?:youtu\.be\/|v=|embed\/|shorts\/)([A-Za-z0-9_-]{11})/);return m?m[1]:(/^[A-Za-z0-9_-]{11}$/.test(u)?u:null)}
function vTime(){try{return player.getCurrentTime()}catch(e){return 0}}
function vPlaying(){try{return kind==='yt'?player.getPlayerState()===1:!player.el.paused}catch(e){return false}}
function vPlay(){lastProg=Date.now();try{kind==='yt'?player.playVideo():player.el.play().catch(()=>{})}catch(e){}}
function vPause(){lastProg=Date.now();try{kind==='yt'?player.pauseVideo():player.el.pause()}catch(e){}}
function vSeek(t){lastProg=Date.now();try{kind==='yt'?player.seekTo(Math.max(0,t),true):(player.el.currentTime=Math.max(0,t))}catch(e){}}
function vRate(r){if(kind==='file'){player.el.playbackRate=r}}
function userPlayed(){ /* the person pressed play on the video */
 if(Date.now()-lastProg<800||cal.on)return;
 if($('vauto').checked&&!run.running){vPause();vSeek(offset);startRun()}}
function loadVideo(){const id=ytId($('yt').value);offset=parseFloat($('off').value)||0;
 if(!id){$('vstat').textContent='not a YouTube link';return}
 const q=new URLSearchParams(location.search);q.set('yt',id);q.set('offset',offset);history.replaceState(null,'','?'+q);
 const make=()=>{ready=false;kind='yt';$('vid').innerHTML='<div id="ytp"></div>';
  player=new YT.Player('ytp',{videoId:id,playerVars:{mute:1,playsinline:1,controls:1,rel:0},
   events:{onReady:()=>{ready=true;player.mute();vSeek(offset);vPause();$('vstat').textContent='ready: press play on the video, or Start lap'},
    onStateChange:e=>{if(e.data===1)userPlayed()},
    onError:e=>{ready=false;$('vstat').textContent=(e.data===101||e.data===150)?'the owner does not allow this video to play outside YouTube; use a file instead':'video error '+e.data}}})};
 if(window.YT&&YT.Player)make();else{window.onYouTubeIframeAPIReady=make;const sc=document.createElement('script');sc.src='https://www.youtube.com/iframe_api';document.head.appendChild(sc)}}
function loadFile(f){if(!f)return;offset=parseFloat($('off').value)||0;
 const v=document.createElement('video');v.src=URL.createObjectURL(f);v.muted=true;v.playsInline=true;v.controls=true;
 $('vid').innerHTML='';$('vid').appendChild(v);kind='file';player={el:v,getCurrentTime:()=>v.currentTime};
 v.addEventListener('play',userPlayed);v.addEventListener('loadedmetadata',()=>{vSeek(offset);ready=true;$('vstat').textContent='ready: press play on the video, or Start lap'})}
function lapT(){if(!R.length)return null;const t=R[R.length-1].t;
 /* rows can arrive in bursts: estimate the lap clock between them, never past the lap's end */
 const extra=run.running&&lastRowWall?Math.min(1.5,(Date.now()-lastRowWall)/1000):0;
 return Math.min(t+extra,lapDur?Math.max(lapDur,t):t+extra)}
function sync(force){if(!ready||cal.on)return;const t=lapT();
 if(t===null||!$('vsync').checked)return;
 /* lap finished: hold the video on the lap's last frame */
 if(lapDur&&R.length&&R[R.length-1].t>=lapDur-0.05){if(vPlaying()){vPause();vSeek(offset+R[R.length-1].t)}vRate(1);return}
 const live=run.running&&Date.now()-lastRowWall<2500;
 if(!live&&!force){vRate(1);return}
 const want=offset+t,cur=vTime(),drift=cur-want;
 if(force||Math.abs(drift)>(kind==='yt'?0.5:0.75)){vSeek(want);vRate(1)}
 else if(kind==='file'){vRate(Math.abs(drift)<0.05?1:1-Math.max(-0.1,Math.min(0.1,drift*0.5)))}
 if(live&&!vPlaying())vPlay();
 $('vstat').textContent=`lap ${t.toFixed(2)} s · video ${(cur-offset).toFixed(2)} s · drift ${drift>=0?'+':''}${drift.toFixed(2)} s`}
setInterval(()=>sync(false),250);
$('vload').onclick=loadVideo;$('vfile').onchange=e=>loadFile(e.target.files[0]);
$('off').onchange=()=>{offset=parseFloat($('off').value)||0;if(ready&&!run.running)vSeek(offset)};
/* ---------- calibration ---------- */
const cal={on:false,brakes:[],down:null};
function calStatus(){$('calst').textContent=`${cal.brakes.length} braking zones marked`+(cal.down!==null?' · braking…':'')}
function calStart(){if(!ready){$('vstat').textContent='load a video first';return}
 if(run.running)stopRun();offset=parseFloat($('off').value)||0;cal.on=true;cal.brakes=[];cal.down=null;
 $('calib').style.display='block';$('cal').disabled=true;setRunUi();vSeek(offset);vPlay();calStatus();
 $('vstat').textContent='calibrating: lap time 0 = video at '+offset.toFixed(1)+' s'}
function bDown(){if(!cal.on||cal.down!==null)return;cal.down=Math.max(0,vTime()-offset);$('hold').classList.add('held');calStatus()}
function bUp(){if(!cal.on||cal.down===null)return;const e=Math.max(0,vTime()-offset);if(e-cal.down>0.15)cal.brakes.push([+cal.down.toFixed(2),+e.toFixed(2)]);cal.down=null;$('hold').classList.remove('held');calStatus()}
async function calFinish(){bUp();const dur=Math.max(0,vTime()-offset);vPause();
 const r=await api('/api/calibrate',{video:ytId($('yt').value)||'file',video_url:$('yt').value,offset,duration:dur,brakes:cal.brakes});
 calEnd();if(!r.ok){$('vstat').textContent='calibration failed: '+r.message;return}
 await loadLaps(r.lap);vSeek(offset);$('vstat').textContent=`lap built: ${cal.brakes.length} braking zones over ${dur.toFixed(1)} s. Press play to run it.`}
function calEnd(){cal.on=false;cal.down=null;$('calib').style.display='none';$('cal').disabled=false;setRunUi()}
$('cal').onclick=calStart;$('fin').onclick=calFinish;$('calx').onclick=()=>{vPause();calEnd()};
$('hold').addEventListener('pointerdown',bDown);$('hold').addEventListener('pointerup',bUp);$('hold').addEventListener('pointerleave',bUp);
addEventListener('keydown',e=>{if((e.key==='b'||e.key==='B')&&!e.repeat&&cal.on){e.preventDefault();bDown()}});
addEventListener('keyup',e=>{if((e.key==='b'||e.key==='B')&&cal.on){e.preventDefault();bUp()}});
/* ---------- boot ---------- */
loadLaps().then(()=>{const q=new URLSearchParams(location.search);if(q.get('yt')){$('yt').value=q.get('yt');$('off').value=q.get('offset')||0;loadVideo()}});
</script></body></html>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir", nargs="?", default="out", help="where runs started here write results")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--qnx", default=os.environ.get("SHADOWLAP_QNX"), help="user@ip of the QNX target, e.g. root@172.31.1.10")
    ap.add_argument("--qnx-dir", default="/data/shadowlap")
    a = ap.parse_args()
    Handler.runner = Runner(a.outdir, a.qnx, a.qnx_dir)
    Handler.outdirs = [a.outdir, "out-qnx"]
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    print(f"Shadow Lap dashboard: http://localhost:{a.port}" + (f"   (QNX target: {a.qnx})" if a.qnx else ""))
    print("Ctrl+C to stop.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        Handler.runner.stop()


if __name__ == "__main__":
    main()
