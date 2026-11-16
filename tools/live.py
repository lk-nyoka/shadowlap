#!/usr/bin/env python3
"""
Live dashboard: watch v1 and v2 while the lap is running.

    python3 tools/live.py out          # in one terminal
    sh scripts/run_demo.sh all out     # in another
    then open http://localhost:8000

It follows whichever out/<scenario>/compare.csv was written to most recently,
so during "all" it switches scenario by itself. Standard library only.

Private testing only: pick a video file from your computer, or paste a
YouTube link in the "Video" box (or open
http://localhost:8000/?yt=<link>&offset=<seconds>) and the video plays in sync
with the lap. "offset" is where the lap starts in the video. The video is
streamed by YouTube's own player and never downloaded. Do not use third-party
footage in the hackathon video or commit links to it in the repo.
"""
import argparse
import glob
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FIELDS = ["seq", "t_lap", "zone", "v_kmh", "soc", "lap_harvest_mj", "v1_regen", "v1_applied",
          "v1_latency_us", "v2_present", "v2_regen", "class", "f_rear_grip", "plant_lag_us"]


def newest_csv(outdir):
    files = glob.glob(os.path.join(outdir, "*", "compare.csv"))
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
            r.append(v if f == "class" else float(v or 0))
        out.append(r)
    return out


class Handler(BaseHTTPRequestHandler):
    outdir = "out"

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/events"):
            return self.events()
        body = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_event(self, kind, data):
        msg = f"event: {kind}\ndata: {json.dumps(data)}\n\n".encode()
        self.wfile.write(msg)
        self.wfile.flush()

    def events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        cur, fp, header, ino = None, None, None, None
        try:
            while True:
                path = newest_csv(self.outdir)
                # same file name but a new run: the file was replaced or truncated
                if path and path == cur and fp:
                    try:
                        st = os.stat(path)
                        if st.st_ino != ino or st.st_size < fp.tell():
                            cur = None
                    except FileNotFoundError:
                        cur = None
                if path and path != cur:
                    if fp:
                        fp.close()
                    cur, fp = path, open(path)
                    ino = os.fstat(fp.fileno()).st_ino
                    header = fp.readline().rstrip("\n").split(",")
                    if len(header) < 5:          # file just created, header not written yet
                        fp.close()
                        cur, fp = None, None
                        time.sleep(0.2)
                        continue
                    self.send_event("reset", {"scenario": os.path.basename(os.path.dirname(path))})
                if fp:
                    rows = rows_from(fp, header)
                    for i in range(0, len(rows), 500):
                        self.send_event("rows", rows[i:i + 500])
                if not fp:
                    self.send_event("waiting", {})
                    time.sleep(1.0)
                time.sleep(0.1)
        except (BrokenPipeError, ConnectionResetError):
            pass


PAGE = r"""<!doctype html><html><head><meta charset="utf-8"><title>Shadow Lap - live</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#0b0b0c;--panel:#141416;--line:#26262a;--txt:#ececea;--mut:#8b8b90;--v1:#3ecf8e;--v2:#ff6b3d;--warn:#f5b13d;--bad:#ff4d4d;--cap:#9b8cff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--txt);font:14px/1.5 Inter,system-ui,-apple-system,Segoe UI,sans-serif}
header{display:flex;align-items:baseline;gap:16px;padding:20px 28px 8px}
h1{font-size:20px;font-weight:600;letter-spacing:-.01em;margin:0}
.scn{font:12px ui-monospace,Menlo,monospace;color:var(--mut);border:1px solid var(--line);padding:3px 10px;border-radius:99px}
.live{width:8px;height:8px;border-radius:50%;background:var(--v1);display:inline-block;margin-right:6px;animation:p 1.2s infinite}
@keyframes p{50%{opacity:.25}}
.stats{display:grid;grid-template-columns:repeat(7,1fr);gap:1px;background:var(--line);margin:12px 28px;border:1px solid var(--line);border-radius:12px;overflow:hidden}
.stat{background:var(--panel);padding:14px 16px}.stat b{display:block;font-size:22px;font-weight:600;font-variant-numeric:tabular-nums;letter-spacing:-.02em}
.stat span{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.06em}
.ok{color:var(--v1)}.bad{color:var(--bad)}.warn{color:var(--warn)}
.grid{display:grid;grid-template-columns:2fr 1fr;gap:16px;padding:8px 28px 28px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px 16px}
.card h2{font-size:12px;font-weight:500;color:var(--mut);margin:0 0 8px;text-transform:uppercase;letter-spacing:.06em}
canvas{width:100%;display:block}
.legend{display:flex;gap:14px;font-size:12px;color:var(--mut);margin-top:6px;flex-wrap:wrap}.legend i{display:inline-block;width:10px;height:3px;margin-right:6px;vertical-align:middle}
#log{list-style:none;margin:0;padding:0;max-height:640px;overflow:auto;font-size:13px}
#log li{padding:10px 0;border-bottom:1px solid var(--line)}#log li em{font-style:normal;font:11px ui-monospace,Menlo,monospace;color:var(--mut);display:block}
.tag{font-size:10px;padding:2px 7px;border-radius:99px;margin-right:6px;color:#0b0b0c;font-weight:600;text-transform:uppercase;letter-spacing:.04em}
.foot{color:var(--mut);font-size:12px;padding:0 28px 24px}
.vid{aspect-ratio:16/9;background:#000;border-radius:8px;overflow:hidden;display:flex;align-items:center;justify-content:center;color:var(--mut);font-size:12px}
.vid iframe{width:100%;height:100%;border:0}
.vrow{display:flex;gap:6px;margin-top:8px}.vrow input{background:#0b0b0c;border:1px solid var(--line);color:var(--txt);border-radius:6px;padding:6px 8px;font-size:12px}
.vrow input#yt{flex:1;min-width:0}.vrow input#off{width:70px}
.vrow button{background:var(--txt);color:#0b0b0c;border:0;border-radius:6px;padding:6px 12px;font-size:12px;font-weight:600;cursor:pointer}
#vstat{font-size:11px;color:var(--mut);margin-top:6px;font-family:ui-monospace,Menlo,monospace}
</style></head><body>
<header><h1>Shadow Lap</h1><span class="scn" id="scn"><span class="live"></span>waiting for a run</span></header>
<div class="stats">
 <div class="stat"><span>Lap time</span><b id="s_t">-</b></div>
 <div class="stat"><span>Speed km/h</span><b id="s_v">-</b></div>
 <div class="stat"><span>Battery</span><b id="s_soc">-</b></div>
 <div class="stat"><span>v1 deadline misses</span><b id="s_miss" class="ok">0</b></div>
 <div class="stat"><span>Host stalls</span><b id="s_ovr">0</b></div>
 <div class="stat"><span>v1 latency now</span><b id="s_lat">-</b></div>
 <div class="stat"><span>v2 status</span><b id="s_v2">-</b></div>
</div>
<div class="grid">
 <div>
  <div class="card"><h2>Speed, coloured where v2 disagrees</h2><canvas id="c1" height="200"></canvas>
   <div class="legend"><span><i style="background:#ff4d4d"></i>rear lock risk</span><span><i style="background:#f5b13d"></i>over battery</span><span><i style="background:#9b8cff"></i>over lap cap</span><span><i style="background:#5aa9e6"></i>different, within limits</span><span><i style="background:#555"></i>v2 not answering</span></div></div>
  <div class="card" style="margin-top:16px"><h2>Motor harvesting, kW</h2><canvas id="c2" height="170"></canvas>
   <div class="legend"><span><i style="background:var(--v1)"></i>v1 (applied to the car)</span><span><i style="background:var(--v2)"></i>v2 (proposed only)</span></div></div>
  <div class="card" style="margin-top:16px"><h2>v1 frame-to-command time (log scale)</h2><canvas id="c3" height="150"></canvas>
   <div class="legend"><span><i style="background:var(--v1)"></i>v1 latency</span><span><i style="background:var(--bad)"></i>8 ms deadline</span></div></div>
 </div>
 <div>
  <div class="card"><h2>Video (private test, synced to lap time)</h2>
   <div class="vid" id="vid">Paste a YouTube link below</div>
   <div class="vrow"><input id="yt" placeholder="YouTube link"><input id="off" type="number" step="0.1" value="0" title="seconds into the video where the lap starts"><button id="vload">Load</button></div>
   <div class="vrow"><label style="font-size:12px;color:var(--mut);cursor:pointer"><input id="vsync" type="checkbox" checked> keep video synced to the lap</label></div>
   <div class="vrow"><label style="font-size:12px;color:var(--mut);cursor:pointer">or play a file from this computer: <input id="vfile" type="file" accept="video/*" style="font-size:12px;color:var(--mut)"></label></div>
   <div id="vstat">offset = seconds into the video where the lap starts</div></div>
  <div class="card" style="margin-top:16px"><h2>What v2 would have done</h2><ul id="log" style="max-height:360px"></ul></div>
 </div>
</div>
<div class="foot">Battery, motor and forces are a simulated model. v2's decisions are never applied to the car.</div>
<script>
const COL={rear_lock_risk:'#ff4d4d',over_battery:'#f5b13d',over_lap_cap:'#9b8cff',within_limits:'#5aa9e6',shadow_missing:'#555'};
const TXT={rear_lock_risk:'asked the rear tyres for more grip than they had',over_battery:'tried to charge harder than the nearly full battery accepts',over_lap_cap:'kept harvesting after the lap energy limit',within_limits:'chose a different split, within limits',shadow_missing:'stopped answering (crashed, frozen or behind)'};
let ovr=0,seen=new Set(),R=[],misses=0,lastCls='agree',runStart=null,dirty=true;
const $=id=>document.getElementById(id);
function reset(s){ovr=0;seen=new Set();R=[];misses=0;lastCls='agree';$('log').innerHTML='';$('scn').innerHTML='<span class="live"></span>'+s.scenario;dirty=true}
let lastRowWall=0;
function add(rows){if(rows.length)lastRowWall=Date.now();for(const r of rows){const [seq,t,zone,v,soc,mj,r1,app,lat,p2,r2,cls,grip,lag]=r;if(lag>2000)ovr++;
  const o={seq,t,zone,v,soc,mj,k1:r1*v/3.6/1e3,app,lat,p2,k2:p2?r2*v/3.6/1e3:null,cls};R.push(o);if(!app)misses++;
  const key=cls+'@'+zone;
  if(cls!==lastCls&&cls!=='agree'&&cls!=='within_limits'&&!(zone>=0&&seen.has(key))){seen.add(key);const li=document.createElement('li');
   li.innerHTML=`<span class="tag" style="background:${COL[cls]}">${cls.replace(/_/g,' ')}</span>v2 ${TXT[cls]}<em>${t.toFixed(1)} s · ${zone>=0?'braking zone '+(zone+1):'on throttle'} · ${v.toFixed(0)} km/h · battery ${(soc*100).toFixed(0)}%</em>`;
   $('log').prepend(li)}
  lastCls=cls}dirty=true}
function setup(c){const d=devicePixelRatio||1,w=c.clientWidth,h=c.height;if(c.width!==w*d){c.width=w*d;c.style.height=h+'px';c._h=h}const g=c.getContext('2d');g.setTransform(d,0,0,d,0,0);g.clearRect(0,0,w,c._h||h);return [g,w,c._h||h]}
function axes(g,w,h,ymin,ymax,ticks,fmt){g.strokeStyle='#26262a';g.fillStyle='#8b8b90';g.font='11px system-ui';g.lineWidth=1;
 for(const y of ticks){const py=h-18-(y-ymin)/(ymax-ymin)*(h-26);g.beginPath();g.moveTo(36,py);g.lineTo(w,py);g.stroke();g.fillText(fmt(y),0,py+4)}}
function X(t,w,tmax){return 36+t/tmax*(w-40)}
function draw(){if(!dirty||!R.length){requestAnimationFrame(draw);return}dirty=false;
 const last=R[R.length-1],tmax=Math.max(90,last.t+1);
 $('s_t').textContent=last.t.toFixed(1)+' s';$('s_v').textContent=last.v.toFixed(0);$('s_soc').textContent=(last.soc*100).toFixed(0)+'%';
 $('s_miss').textContent=misses;$('s_ovr').textContent=ovr;$('s_ovr').className=ovr?'warn':'ok';$('s_miss').className=misses?'bad':'ok';
 $('s_lat').textContent=last.lat>=0?(last.lat<1000?last.lat.toFixed(0)+' µs':(last.lat/1000).toFixed(1)+' ms'):'missed';
 const v2=$('s_v2');if(!last.p2){v2.textContent='not answering';v2.className='warn'}else if(last.cls==='agree'){v2.textContent='agrees';v2.className='ok'}else{v2.textContent='disagrees';v2.className='bad'}
 let [g,w,h]=setup($('c1'));axes(g,w,h,0,340,[0,100,200,300],y=>y);
 g.lineWidth=1.5;g.strokeStyle='#ececea';g.beginPath();R.forEach((o,i)=>{const x=X(o.t,w,tmax),y=h-18-o.v/340*(h-26);i?g.lineTo(x,y):g.moveTo(x,y)});g.stroke();
 for(const o of R){if(o.cls!=='agree'){g.fillStyle=COL[o.cls];g.fillRect(X(o.t,w,tmax)-1,h-18-o.v/340*(h-26)-2,3,4)}}
 [g,w,h]=setup($('c2'));axes(g,w,h,0,360,[0,100,200,300],y=>y);
 for(const [k,c] of [['k1','#3ecf8e'],['k2','#ff6b3d']]){g.strokeStyle=c;g.lineWidth=k==='k1'?1.8:1.2;g.setLineDash(k==='k2'?[4,3]:[]);g.beginPath();let pen=false;
  for(const o of R){if(o[k]===null){pen=false;continue}const x=X(o.t,w,tmax),y=h-18-o[k]/360*(h-26);pen?g.lineTo(x,y):g.moveTo(x,y);pen=true}g.stroke()}g.setLineDash([]);
 [g,w,h]=setup($('c3'));const L=v=>Math.log10(Math.max(v,10));axes(g,w,h,1,4.3,[1,2,3,4],y=>({1:'10µs',2:'100µs',3:'1ms',4:'10ms'})[y]);
 const dy=h-18-(L(8000)-1)/3.3*(h-26);g.strokeStyle='#ff4d4d';g.setLineDash([5,4]);g.beginPath();g.moveTo(36,dy);g.lineTo(w,dy);g.stroke();g.setLineDash([]);
 g.fillStyle='#3ecf8e';for(const o of R){const x=X(o.t,w,tmax);if(o.lat<0){g.fillStyle='#ff4d4d';g.fillRect(x-1,dy-6,3,12);g.fillStyle='#3ecf8e';continue}g.fillRect(x,h-18-(L(o.lat)-1)/3.3*(h-26),1.2,1.2)}
 requestAnimationFrame(draw)}
const es=new EventSource('/events');es.addEventListener('reset',e=>reset(JSON.parse(e.data)));es.addEventListener('rows',e=>add(JSON.parse(e.data)));
es.addEventListener('waiting',()=>{$('scn').innerHTML='<span class="live"></span>waiting for a run'});
addEventListener('resize',()=>{dirty=true});requestAnimationFrame(draw);

/* ---- optional synced YouTube video (private testing) ---- */
let player=null,ready=false,offset=0;
function ytId(u){u=(u||'').trim();let m=u.match(/(?:youtu\.be\/|v=|embed\/|shorts\/)([A-Za-z0-9_-]{11})/);return m?m[1]:(/^[A-Za-z0-9_-]{11}$/.test(u)?u:null)}
function lapT(){return R.length?R[R.length-1].t:null}
function loadVideo(){const id=ytId($('yt').value);offset=parseFloat($('off').value)||0;
 if(!id){$('vstat').textContent='not a YouTube link';return}
 const q=new URLSearchParams(location.search);q.set('yt',id);q.set('offset',offset);history.replaceState(null,'','?'+q);
 const make=()=>{ready=false;$('vid').innerHTML='<div id="ytp"></div>';
  player=new YT.Player('ytp',{videoId:id,playerVars:{mute:1,playsinline:1,controls:1,rel:0},
   events:{onReady:()=>{ready=true;player.mute();sync(true)},
    onError:e=>{ready=false;$('vstat').textContent=(e.data===101||e.data===150)?
     'the owner does not allow this video to play outside YouTube; use a file instead':'video error '+e.data}}})};
 if(window.YT&&YT.Player)make();else{window.onYouTubeIframeAPIReady=make;const sc=document.createElement('script');sc.src='https://www.youtube.com/iframe_api';document.head.appendChild(sc)}}
/* local file: same interface as the YouTube player */
function loadFile(f){if(!f)return;offset=parseFloat($('off').value)||0;
 const v=document.createElement('video');v.src=URL.createObjectURL(f);v.muted=true;v.playsInline=true;v.style.cssText='width:100%;height:100%';
 $('vid').innerHTML='';$('vid').appendChild(v);
 player={getCurrentTime:()=>v.currentTime,seekTo:t=>{v.currentTime=Math.max(0,t)},getPlayerState:()=>v.paused?2:1,playVideo:()=>v.play().catch(()=>{}),mute:()=>{}};
 ready=true;sync(true)}
function sync(force){if(!ready||!player.getCurrentTime)return;const t=lapT();
 if(t===null){$('vstat').textContent='waiting for the lap to start';return}
 if(!$('vsync').checked){$('vstat').textContent='sync off: the video plays freely';return}
 /* lap not moving (finished or stopped): leave the video alone so it can be paused */
 if(Date.now()-lastRowWall>1500){$('vstat').textContent=`lap stopped at ${t.toFixed(1)} s · video is free to pause or scrub`;return}
 const want=offset+t,cur=player.getCurrentTime(),drift=cur-want;
 if(force||Math.abs(drift)>0.4){player.seekTo(want,true)}
 if(player.getPlayerState()!==1)player.playVideo();
 $('vstat').textContent=`lap ${t.toFixed(1)} s · video ${cur.toFixed(1)} s · drift ${drift>=0?'+':''}${drift.toFixed(2)} s`}
setInterval(()=>sync(false),1000);
es.addEventListener('reset',()=>{lastRowWall=0;if(ready)setTimeout(()=>sync(true),300)});
$('vload').onclick=loadVideo;
$('vfile').onchange=e=>loadFile(e.target.files[0]);
$('off').onchange=()=>{offset=parseFloat($('off').value)||0;sync(true)};
{const q=new URLSearchParams(location.search);if(q.get('yt')){$('yt').value=q.get('yt');$('off').value=q.get('offset')||0;loadVideo()}}
</script></body></html>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir", nargs="?", default="out")
    ap.add_argument("--port", type=int, default=8000)
    a = ap.parse_args()
    Handler.outdir = a.outdir
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    print(f"Live dashboard: http://localhost:{a.port}   (watching {a.outdir}/*/compare.csv, Ctrl+C to stop)")
    srv.serve_forever()


if __name__ == "__main__":
    main()
