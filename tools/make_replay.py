#!/usr/bin/env python3
"""
Build a no-install replay of a recorded run: the real dashboard page
(tools/live.py), with a small shim that serves the run's own recorded data
instead of a live server, played back at real speed.

    python3 tools/make_replay.py RUN_DIR --lap data/lap_openf1_monza_2024_4.csv \
        --target qnx --scenario shadow --candidate v2 --cond tough \
        --recorded "QNX SDP 8.0 in QEMU, 18 November 2026" --out docs/replay/index.html

RUN_DIR must hold the run's compare.csv. Nothing in the dashboard is redrawn
or changed: rows go through the same code path as a live run.
Standard library only.
"""
import argparse
import base64
import gzip
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import live  # noqa: E402


SHIM = r"""<script>
/* Replay shim: serves one recorded run to the unchanged dashboard below. */
(function(){
const REC=__REC__;
let rows=null, idx=0, t0=0, timer=null, running=false, lastStatus=0, ES=null;
const ready=(async()=>{
  const b=Uint8Array.from(atob(REC.rows_gz),c=>c.charCodeAt(0));
  const s=new Blob([b]).stream().pipeThrough(new DecompressionStream('gzip'));
  rows=JSON.parse(await new Response(s).text());
})();
function state(){return {running, target:REC.target, scenario:REC.scenario, lap:REC.lap, candidate:REC.candidate,
  cond:REC.cond, lap_duration:REC.lap_duration, message: running?'replaying the recorded run':(idx?'replay ended':'ready')}}
function J(d){return new Response(JSON.stringify(d),{headers:{'Content-Type':'application/json'}})}
function emit(k,d){if(ES&&ES.h[k])ES.h[k].forEach(f=>f({data:JSON.stringify(d)}))}
async function play(){
  await ready; stop(true); idx=0; running=true;
  emit('reset',{scenario:REC.scenario, source:REC.target==='qnx'?'qnx':'local'}); emit('status',state());
  t0=performance.now();
  timer=setInterval(()=>{
    const want=Math.min(rows.length, Math.floor((performance.now()-t0)/1000*REC.rate));
    if(want>idx){emit('rows',rows.slice(idx,want)); idx=want}
    if(performance.now()-lastStatus>500){emit('status',state()); lastStatus=performance.now()}
    if(idx>=rows.length){stop()}
  },50);
}
function stop(silent){if(timer)clearInterval(timer); timer=null; if(running){running=false; if(!silent)emit('status',state())}}
const realFetch=window.fetch.bind(window);
window.fetch=async function(u,o){u=String(u);
  if(u.startsWith('/api/laps'))return J({laps:[REC.lapinfo], scenarios:[REC.scenario], qnx:REC.qnx_label});
  if(u.startsWith('/api/lapgeo'))return J(REC.geo);
  if(u.startsWith('/api/status'))return J(state());
  if(u.startsWith('/api/start')){play(); return J({ok:true, message:'replaying', state:Object.assign(state(),{running:true})})}
  if(u.startsWith('/api/stop')){stop(); return J({ok:true, state:state()})}
  if(u.startsWith('/api/calibrate'))return J({ok:false, message:'not available in the replay'});
  return realFetch(u,o)};
window.EventSource=function(){ES=this; this.h={};
  this.addEventListener=(k,f)=>{(this.h[k]=this.h[k]||[]).push(f)}; this.close=()=>{};
  setTimeout(()=>emit('status',state()),0);
  setTimeout(()=>{try{const s=(id,v)=>{const e=document.getElementById(id); if(e){e.value=v; e.disabled=true}};
    s('lap',REC.lap); s('target',REC.target); s('scen',REC.scenario); s('cand',REC.candidate); s('cond',REC.cond);
    play();}catch(e){}},800);};
})();
</script>
"""

BANNER = """<div style="background:#132033;border-bottom:1px solid #2d5d93;color:#e8edf3;font:13px/1.5 Inter,system-ui,sans-serif;padding:10px 24px">
<b>Replay of a real recorded run</b>: __RECORDED__. This is the Shadow Lap dashboard itself; the data is that run's own
<code>compare.csv</code>, played back at real speed (press <b>Start lap</b> to replay again). Nothing here runs on QNX now.
&nbsp;<a style="color:#5aa9ff" href="https://github.com/lk-nyoka/shadowlap">Code (MIT)</a> ·
<a style="color:#5aa9ff" href="https://youtu.be/zPbI2RrV_hg">3-minute video</a> ·
Lap data: <a style="color:#5aa9ff" href="https://openf1.org">OpenF1</a>, an independent project; the lap and run data on this page are
shared under <a style="color:#5aa9ff" href="https://creativecommons.org/licenses/by-nc-sa/4.0/">CC BY-NC-SA 4.0</a> (non-commercial).
Not affiliated with or endorsed by Formula 1 or any team.</div>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--lap", required=True)
    ap.add_argument("--target", default="qnx")
    ap.add_argument("--scenario", default="shadow")
    ap.add_argument("--candidate", default="v2")
    ap.add_argument("--cond", default="tough")
    ap.add_argument("--recorded", required=True, help="where and when it was recorded, shown on the page")
    ap.add_argument("--out", default="docs/replay/index.html")
    a = ap.parse_args()

    lap_name = os.path.splitext(os.path.basename(a.lap))[0]
    dst = os.path.join(live.ROOT, "data", lap_name + ".csv")
    if os.path.abspath(a.lap) != os.path.abspath(dst):
        shutil.copy(a.lap, dst)
    info = live.lap_info(dst)
    geo = live.lap_geometry(lap_name)
    with open(os.path.join(a.run, "compare.csv")) as fp:
        header = fp.readline().rstrip("\n").split(",")
        rows = live.rows_from(fp, header)
    if not rows:
        sys.exit("no rows in compare.csv")
    if a.cond not in live.CONDITIONS:
        sys.exit(f"--cond must be one of {list(live.CONDITIONS)}")
    rec = {
        "rows_gz": base64.b64encode(gzip.compress(json.dumps(rows, separators=(",", ":")).encode(), 9)).decode(),
        "rate": 100, "target": a.target, "scenario": a.scenario, "candidate": a.candidate, "cond": a.cond,
        "lap": lap_name, "lapinfo": info, "lap_duration": info["duration"], "geo": geo,
        "qnx_label": "recorded run" if a.target == "qnx" else None,
    }
    page = live.PAGE
    page = page.replace("<title>Shadow Lap - live</title>", "<title>Shadow Lap - replay of a recorded QNX run</title>")
    page = page.replace("</head>", SHIM.replace("__REC__", json.dumps(rec, separators=(",", ":"))) + "</head>", 1)
    page = page.replace("<body>", "<body>" + BANNER.replace("__RECORDED__", a.recorded), 1)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w") as fp:
        fp.write(page)
    print(f"wrote {a.out}: {len(rows)} frames, {os.path.getsize(a.out) // 1024} KB")


if __name__ == "__main__":
    main()
