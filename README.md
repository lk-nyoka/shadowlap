# Shadow Lap

**Test the next brake controller without putting it in control. A QNX 8.0
prototype.**

Every road-car maker that blends motor braking with disc braking, and every
racing team, has the same problem: you can't let untested control software
brake a real car. Shadow Lap runs the candidate controller (v2) beside the
trusted one (v1). v2 sees every input and makes every decision, but its
decisions only go into a log. v1 keeps sole control of the actuator. In our
tests, none of the injected candidate failures (crashing, hanging, being
stopped, flooding its CPU, attacking the brakes) made v1 miss a deadline.

The output is evidence: every moment the candidate broke a limit or
disagreed, why, how serious it was, and a verdict that fails closed.

**Shadow mode is not new.** Running old and new vehicle software side by
side and comparing them is described in patents such as Bosch US 12,386,724
and Hitachi Astemo US 12,536,094 (which also covers running the new version
on a different processor). Commercial brake test benches and
hardware-in-the-loop systems exist too. What Shadow Lap offers is an **open, QNX-based reference
implementation for brake blending** that tests its own containment: a
hostile candidate is attacked against the trusted side route by route and
the results are measured, the limit checks are absolute, and the evidence
gate fails closed. It is meant to sit alongside existing HIL tools, not
replace them.

> Entry for the QNX Global Hackathon 2026, category **Automotive & Mobility**.

## Why: braking software updates go wrong

In 2025 Volvo recalled more than 11,000 US plug-in hybrids and EVs (NHTSA
recall 25V392): with Brake Control Module software 3.5.14, braking could be
lost after about 100 seconds of coasting downhill in "B" mode or One Pedal
Drive.

Shadow Lap's third candidate, **v3**, is a *Volvo-inspired* example of that
class of failure, written on purpose: when motor braking is unavailable
(battery nearly full, or the lap's energy limit used up), it does not hand the
full demand to the friction brakes. The `demand_not_met` check catches it.
v3 does not reproduce Volvo's actual defect, its 100-second trigger or its
controller; we do not claim Shadow Lap would have caught that recall.

## What it looks like

The dashboard replays a **real F1 lap** (Monza 2024 qualifying pole, 1:19.327,
from free public timing data) and shows, moment by moment:

- **the car on a map of the real circuit**, drawn from its position data, with
  braking zones marked and dots where the candidate broke a limit;
- **the rear brakes right now**: what the driver asks for, how v1 (in charge)
  splits it between motor and discs, how the candidate would have split it,
  against the tyre-grip limit;
- **one plain sentence** saying what is happening, e.g. *"Braking from 210 km/h
  at 4.3 g. v2 would ask the rear tyres for more than they can grip: the rear
  wheels could lock. v1 stays under the limit."*

Pick the **conditions** at the top (normal day, worn tyres, hot day, or both)
and the **candidate** (v2, v2.1, v3). On a normal day v2's flaws stay hidden;
with hot weather and worn tyres the same lap shows v2 risking a rear lock at
the first braking zone (about 9 s in) and over-asking a hot motor later.

**What is real and what is modelled.** From OpenF1 (an independent project
not associated with Formula 1; licensed CC BY-NC-SA 4.0, for non-commercial
use; about 3.7 samples a second): speed, throttle, brake
on/off, track position, lap time. Shadow Lap interpolates that to a 100 Hz
replay, so the 10 ms frames are simulation frames, not 100 Hz measurements,
and the braking g is derived from smoothed speed. Battery, motor, wings, grip
and forces are a small model inspired by the 2026 rules (350 kW motor; the
2.4 MJ lap harvest limit is a scaled, illustrative number, not the FIA's
8.5 MJ). Grip and downforce are fitted so the real pole lap uses up to 93% of
the modelled grip at its hardest braking. Mixing a 2024 lap with 2026-style
parameters is a modelling choice, not an FIA-compliant car.

### Many laps: what the checks found

`bbw_sweep` replays the real lap 600 times (200 runs × 3 laps) with battery
50–92%, air 10–45 °C and track grip 85–105%, in under a second. **Hard limits**
(rear lock risk, braking demand not met) count from a single 10 ms frame;
**soft limits** (battery, lap energy, motor heat) once they last 50 ms. The
same checks run on v1.

| Finding | Depends on | Result |
|---|---|---|
| v2 risks locking the rear wheels | track grip | 100% of laps below 99% grip, 0% at normal grip; worst request 16% over the grip limit |
| v2 over-asks a hot motor | air temperature | 37% of laps at 10–25 °C, 67% at 25–35 °C, 96% at 35–45 °C |
| v2 over-asks a hot motor | lap of the run | 27% on lap 1, 84% on lap 2, 82% on lap 3 |
| v3 brakes less than asked | lap of the run | 92% of first laps, 30% of later laps |
| v1 (the baseline) | | broke no limit on any frame; peak rear grip use 95% (v1 caps itself at 95% of grip, so that figure is by construction) |
| v3's consequence | | worst case: the car still **13.8 km/h faster** at the end of a braking zone, **6 m further** down the road (estimated from the missing braking; v1 and v2.1: 0) |

| Candidate | Laps with a limit break | Gate verdict |
|---|---|---|
| v2 | 600 of 600 (hard limit on 420) | **FAIL** |
| v3 | 304 of 600 (all hard: demand not met) | **FAIL** |
| v2.1 (v2 with its flaws fixed) | 0 of 600, peak grip use 95% | **PASS_WITHIN_SCOPE** |

PASS_WITHIN_SCOPE means "no limit broken in this campaign": these laps, these
condition ranges, this car model. Zero failures in 600 related laps is not a
safety proof; it is evidence for the next validation stage (hardware in the
loop, then a supervised vehicle test).

**Open-loop vs counterfactual.** In shadow mode v2 sees the car v1 drives, so
v2's own decisions never shape the battery, lap energy or motor heat it sees.
`bbw_sweep --mode counterfactual` lets v2 drive its own copy of the car. On
this lap it changed little (v2's hot-motor laps went from 387 to 406 of 600;
v2.1 still passed), because the car model refuses to let a motor exceed its
limits either way.

### Results on QNX 8.0

Measured 16 November on the current code. Real Monza lap (7,933 frames, one
every 10 ms), hot day with worn tyres (grip 90%, 42 °C, battery 88%), QNX SDP
8.0 in QEMU (2 CPUs) on a Windows 11 laptop. The car and v1 pinned to CPU 0
at priorities 70 and 65; v2 on CPU 1 as the unprivileged user `qnxuser`.

A **v1 miss** means v1 had its full 8 ms after receiving a frame and still
didn't answer. A **host stall** means the simulated car itself was woken more
than 2 ms late (QNX here runs inside a VM on Windows, which can still pause it).

| Scenario | v1 misses | Host stalls | v1 p99 | v1 worst |
|---|---|---|---|---|
| baseline (no shadow) | 0 | 27 | 207 µs | 0.67 ms |
| shadow | 0 | 0 | 168 µs | 1.22 ms |
| v2 crashes | 0 | 0 | 209 µs | 0.62 ms |
| v2 hangs 2 s | 0 | 0 | 160 µs | 0.59 ms |
| v2 stopped from outside 2 s (SIGSTOP)* | 0 | 0 | 190 µs | 0.54 ms |
| CPU flood on the shadow side | 0 | 4 | 99 µs | 3.11 ms |
| hostile v2 (7 routes, then burns v1's CPU at priority 63; stopped by the guard)* | 0 | 0 | 223 µs | 0.76 ms |

Every scenario: 0 actuator commands rejected, 0 records dropped, 0 clamps.
\* Re-run 16 November after the launcher fix and the CPU-budget guard. The
SIGSTOP run now pauses v2 itself (the same process ID the guard watched); in
the attack run the guard stopped v2 when it reached 98% of a CPU (budget 40%).
The earlier version (15 November, Track A, v1 at priority 45) also gave 0
misses in all seven; one first shadow run then gave 2 misses with 337 host
stalls, which we read as the Windows host being busy.

**The attack test, and the weakness it found.** A hostile v2,
running as `qnxuser`, tries seven routes and reports each. QNX blocked five:
the brake actuator, v1's memory, the evidence, writing its own inputs, and
signalling v1. Two were open: v2 could raise its priority above v1's (then
45), and move onto v1's CPU. Together that would let it take v1's core.

The cause is in QNX's design and so is the fix: unprivileged processes can
use priorities 1 to 63, and 64 and up need the `PROCMGR_AID_PRIORITY`
ability. The car (70) and v1 (65) now sit in that privileged band on QNX, so
v2 tops out at 63, below both, whatever it does. The attack now also proves
it: v2 climbs to the highest priority it can reach, moves onto v1's CPU and
burns it for 30 seconds while the lap runs.

**A CPU budget for the candidate.** Priority alone means v2 could still hog a
core below 64. QNX 8.0 discontinued adaptive partitioning, so `bbw_guard`
uses what QNX 8.0 offers instead: it reads the candidate's CPU-time clock
from outside every 50 ms, at priority 64 (above anything v2 can reach), and
stops the candidate if it uses more than 40% of a CPU over a second. It is a
watchdog with a measured containment time (it logs how long the candidate ran
hot before being stopped; the design limit is about 1.05 s), not an
instantaneous CPU reservation. If the guard's log is incomplete, the report
marks that run's evidence as incomplete. On
Linux the hostile v2 was stopped about a second into its CPU burn (98% of a
CPU), with 0 v1 misses. **On QNX:** `[guard] BUDGET EXCEEDED: pid 2715673 used
98% of a CPU over the last 1000 ms (budget 40%) -> candidate stopped
(SIGKILL)`, with 0 v1 deadline misses in 7,933 frames.

**A test that was not testing what it said.** QNX's `on -u` (and Linux's
`runuser`) start the candidate as a *child* process, so the script's process
ID was the launcher's. The old SIGSTOP scenario therefore paused the
launcher, not v2. `bbw_runas` now drops to the unprivileged user and then
*becomes* v2 (same process ID). On Linux the corrected scenario really pauses
v2 (99 answers missing, recorded as gaps; 0 v1 misses). Re-run on QNX with
the fix: 0 v1 misses (table above).

**Re-run on QNX with the fix (16 November):**

```
[v2] ATTACK BLOCKED: open the brake actuator /dev/bbw/actuator -> Permission denied
[v2] ATTACK BLOCKED: open v1's region /bbw_prod -> Permission denied
[v2] ATTACK BLOCKED: open the evidence region /bbw_rec -> Permission denied
[v2] ATTACK BLOCKED: open its input region /bbw_shin for writing -> Permission denied
[v2] ATTACK BLOCKED: signal v1 (stop or kill it) -> Operation not permitted
[v2] ATTACK BLOCKED: raise its priority above v1's -> Operation not permitted
[v2] note: highest priority v2 can reach is 63; v1 runs at 65, the car at 70
[v2] ATTACK CONTAINED: move onto v1's CPU -> allowed, but v1 (65) and the car (70) out-rank anything v2 can reach (63)
[v2] attack: spinning on v1's CPU at priority 63 for 30 s while the lap runs
[plant] done: 8914 cycles, 0 v1 deadline misses, 10 plant overruns (host/OS late)
```

v1 latency during the attack lap: p99 162 µs, worst 1.64 ms (deadline 8 ms).

### The same laptop without QNX

| | Linux in WSL2 (a Hyper-V VM, real-time priority) | QNX SDP 8.0 in QEMU |
|---|---|---|
| v1 deadline misses (shadow lap) | 40 | **0** |
| Host stalls | 82 | 48 |
| v1 worst response | 7.96 ms | **1.96 ms** |

This is an observation about these two setups, both running on top of the
Windows host scheduler, not proof that Linux cannot do real time (PREEMPT_RT
Linux on the same hardware would be the fair comparison). On Linux in a cloud
container v1 also stayed at 0–2 misses.

## Built during the hackathon

The repository was created on 16 November 2026, after the hackathon opened
(2 November); every commit is in its history. In order: the five-process QNX
system and the seven scenarios; QNX 8.0 results; motor and wing physics, the
many-lap sweep and the release gate; the real Monza lap with the track map;
then, after two rounds of design review, the fail-closed gate, read-only
candidate inputs, absolute limit checks and candidate v3, the privileged
priority band (found by the QNX attack test), the CPU-budget guard and the
`bbw_runas` launcher.

## How it works

Five separate processes. Full plain-English walkthrough:
[docs/how-it-works.md](docs/how-it-works.md).

```
 lap replay ─► bbw_plant (the car, owns the clock and the actuator)
                 │  frame every 10 ms
        ┌────────┴─────────────┐
        ▼                      ▼  read-only broadcast ring (v2 cannot write it)
     bbw_v1 ──write()──►    bbw_v2 (shadow: watches only)
   /dev/bbw/actuator           │ proposals
   (only v1 can open)          ▼
                         bbw_comparer ─► compare.csv, summary.json ─► report.html
```

QNX features used:

- **Resource manager**: the brake actuator is `/dev/bbw/actuator`. v1's
  `write()` is a QNX message, and the plant thread handling it inherits v1's priority.
- **Process isolation + shared memory**: four regions. v1's region and the
  evidence are closed to v2; v2 maps its inputs read-only; its output region
  is treated as untrusted and checked by the comparer.
- **Fixed priorities** (`SCHED_FIFO`) with the car (70) and v1 (65) in QNX's
  privileged band (64+), out of reach of an unprivileged v2 (max 63), and
  **CPU pinning** (`ThreadCtl` runmask).
- **Permissions**: v2 runs as another user and is denied the actuator, v1's
  memory and the evidence. A QNX security policy (prepared, not yet run) is
  the stronger next step: [docs/security-policy.md](docs/security-policy.md).
- **Real-time setup is verified, not assumed**: every safety-relevant thread
  (including the actuator thread) sets and checks its priority and CPU; on
  QNX a run that fails to get real-time scheduling refuses to start
  (`REQUIRE_RT=1`), and the evidence records it.

## Run it

Windows 11 step by step, including QNX in QEMU:
[docs/setup-windows11.md](docs/setup-windows11.md).

Quick start on Linux or WSL (no QNX needed):

```bash
make && make test
pip install numpy matplotlib
python3 tools/make_synthetic_lap.py          # writes data/lap_a.csv
sh scripts/run_demo.sh all out               # ~11 min: seven full laps
python3 tools/report.py out                  # writes out/report.html
```

**AI review:** `python3 tools/analyst.py out/shadow` writes an engineering
verdict on v2 from the run's evidence, with every number checked against the
data. Free options (local Ollama or a free Gemini key): [docs/ai-analyst.md](docs/ai-analyst.md).

**Many laps and the release-evidence gate:**

```bash
build/host/bbw_sweep --runs 200 --laps 3 --out out/sweep                       # candidate v2
build/host/bbw_sweep --runs 200 --laps 3 --candidate v2.1 --out out/sweep-v2.1
build/host/bbw_sweep --runs 200 --laps 3 --candidate v2.1 --mode counterfactual --out out/sweep-v2.1-cf
python3 tools/gate.py out/sweep/summary.json          # FAIL, with reasons (exit 1)
python3 tools/gate.py out/sweep-v2.1/summary.json     # PASS_WITHIN_SCOPE (exit 0)
python3 tools/report.py out                           # adds the many-laps charts
```

The gate has three verdicts: **PASS_WITHIN_SCOPE**, **FAIL**, and **INVALID**
(missing or inconsistent evidence, the wrong number of laps or frames,
missing candidate answers, or v1 itself breaking a limit). Anything but a
pass exits 1. It writes `manifest.json` beside the summary: lap-file, tool,
controllers and monitor hashes, seed, ranges, git commit. The monitor's hash
must match `tools/monitor.sha256`, or the verdict is INVALID, so a change to
the checks cannot slip in unnoticed. `python3 tests/test_gate.py` feeds it 18
broken, incomplete and unsafe summaries to prove it fails closed.

GitHub runs all of this on every push (`.github/workflows/shadow-gate.yml`)
and requires v2 and v3 to FAIL and v2.1 to get PASS_WITHIN_SCOPE under the
pinned campaign (seed 1, 300 laps of Track A), so CI also proves the gate can
reject a bad candidate.

Run the live system with another candidate or different conditions:
`CANDIDATE=v3 sh scripts/run_demo.sh shadow out` or
`PLANT_ARGS="--laps 3 --ambient 40 --grip 0.9" sh scripts/run_demo.sh shadow out`.

**The dashboard (recommended):**

```bash
python3 tools/live.py --qnx root@172.31.1.10     # leave out --qnx to run on this PC only
```

Open http://localhost:8000. Pick a lap, a scenario and where to run it
(this PC or QNX), then press **Start lap**, or just press play on a loaded
video: the lap starts by itself and the video stays in step with the charts.
**Stop** ends the run on either machine.

To make the charts match a video: load it, set the offset (the second the lap
starts), click **Calibrate lap from this video**, hold **B** whenever the car
brakes, and click **Finish**. The lap is rebuilt so its braking happens exactly
when the video's does (speeds are modelled). It then appears in the lap list.

On QNX (after `source ~/qnx800/qnxsdp-env.sh`):

```bash
make TARGET=qnx bundle                       # QEMU x86_64
make TARGET=qnx QNX_ARCH=aarch64le bundle    # Raspberry Pi
# copy build/qnx-*/bundle to the target, then on the target:
V2_USER=qnxuser PROD_CPU=0 SHADOW_CPU=1 sh run_demo.sh all out
```

Build and run details:

- **QNX version**: QNX SDP 8.0
- **Target**: QEMU x86_64 (`mkqnximage --type=qemu`); Raspberry Pi 4/5 should work with `QNX_ARCH=aarch64le`
- **Toolchain**: `qcc -Vgcc_ntox86_64` / `-Vgcc_ntoaarch64le`

Get the real lap (free, no account; files stay out of the repo):

```bash
python3 tools/fetch_openf1_lap.py --year 2024 --circuit Monza --session Qualifying
build/host/bbw_sweep --lap data/lap_openf1_monza_2024_4.csv --runs 200 --laps 3 --out out/sweep
```

The dashboard picks a real lap automatically when one is in `data/`.

Or with FastF1 (may now need a free F1TV login; not tested yet):

```bash
pip install fastf1
python3 tools/fetch_fastf1_lap.py --year 2024 --event Monza --session Q --out data/lap_real.csv
LAP=data/lap_real.csv sh scripts/run_demo.sh shadow out
```

## Repo map

| Path | What |
|---|---|
| `include/bbw.h` | messages, never-blocking rings (including the read-only broadcast ring), shared regions |
| `include/control.h`, `src/control.c` | car limits (battery, motor torque and heat, grip, wings), v1, v2, v2.1 and v3 |
| `include/monitor.h`, `src/monitor.c` | the safety monitor: limit specification, absolute checks, consequence estimate; hash pinned in `tools/monitor.sha256` |
| `include/vehicle.h`, `src/vehicle.c` | the simulated car, shared by the live plant and the sweep |
| `src/sweep.c` | `bbw_sweep`: hundreds of laps in varied conditions, in seconds |
| `tools/gate.py`, `.github/workflows/` | release verdict, and GitHub checks on every push |
| `src/plant.c` | simulated car, lap replay, deadline check |
| `src/actuator.c` | QNX resource manager (Linux: FIFO stand-in) |
| `src/v1.c`, `src/v2.c` | the two controllers; v2 has built-in fault injection |
| `src/comparer.c` | joins v1/v2 per frame, writes the evidence |
| `src/hog.c` | CPU burner for the flood test |
| `src/guard.c` | `bbw_guard`: outside CPU budget for the candidate (reads its CPU-time clock) |
| `src/runas.c` | `bbw_runas`: trusted launcher that drops to the candidate's user and becomes it |
| `scripts/run_demo.sh` | the seven scenarios (attack = seven routes, each reported) |
| `tools/` | live dashboard, real-lap import (`fetch_openf1_lap.py`), lap from video (`lapgen.py`), synthetic lap, FastF1 import, HTML report, AI analyst, QNX live link |
| `tests/` | ring stress tests (including torn-read checks), strategy and limit-check tests, the gate's fail-closed tests |

## Honest limits

- **A model, not a car.** Battery, motor, wings, grip and forces are a small
  illustrative model (`include/control.h`). The lap gives speed, throttle, a
  brake on/off flag and position at about 3.7 Hz, interpolated to 100 Hz.
  The car's speed follows the recorded lap whatever the controllers do
  (kinematic replay): the controllers decide only the rear motor/disc split.
- **Seeded flaws.** v2's and v3's flaws were written on purpose by the same
  people who wrote the checks, so catching them shows the pipeline works, not
  that it finds unknown bugs. Blind fault injection (flaws written by someone
  else) is the next test.
- **Checks cover rear lock, braking demand, battery, lap energy and motor
  heat.** Not modelled: yaw and stability, left/right asymmetry, ABS/ESC
  interaction, actuator rate limits, sensor faults, wet tracks, friction
  brake capacity, and long-duration or mode-transition faults (Volvo's
  trigger needed about 100 s of pedal-free regen coasting; our laps contain
  no such state, so a clean pass says nothing about that kind of fault).
- **The checker is separate, but not fully independent.** The limit checks
  live in `src/monitor.c` with their own copy of every limit (cross-checked
  against the car model by a test) and a pinned hash; the per-frame grip and
  braking request still come from the car model.
- **Consequences are estimates.** The replayed speed does not change; the
  extra speed and distance are computed from the braking a candidate did not
  deliver, using the car's mass.
- **Open-loop by nature.** In shadow mode v2 sees the car v1 drives; the
  counterfactual sweep is a first answer to that, using the same model.
- **The deadline-miss fallback is illustrative**: discs only, capped at 95% of
  rear grip. A real car's degraded mode comes from its own hazard analysis.
- **QNX SDP 8.0 in QEMU, not QNX OS for Safety on real hardware.** Nothing
  here is ISO 26262 certified. Verified in QEMU: the actuator resource
  manager, real-time priorities, CPU pinning, v2 as another user. Not yet
  run: the security-policy steps and real hardware (Raspberry Pi).
- **The CPU budget is a watchdog, not a scheduler guarantee.** QNX 8.0 has
  no adaptive partitioning; `bbw_guard` stops an over-budget candidate within
  about a second, so for that second it can still slow services below
  priority 64 on its core. Lowering the unprivileged ceiling (`procnto -P`)
  is a further option.
- **Nested virtualisation.** QNX runs in QEMU inside WSL2 (itself a Hyper-V
  VM) on Windows, so timing includes two layers of host scheduling.
- **Shadow mode is not new.** The contribution is the QNX implementation, the
  isolation tests, the absolute checks and the fail-closed gate.
- **Data licence.** OpenF1 is unofficial and for non-commercial use; the lap
  data is downloaded by the user and not stored in this repo. F1 and related
  marks belong to Formula One Licensing B.V.
- The AI analyst only explains evidence; it never decides a verdict. Its
  number checker is tested; a review from a real model is not yet in the repo.

## License

MIT. See [LICENSE](LICENSE).
