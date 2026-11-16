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

Shadow-mode testing of vehicle software is established practice (Bosch, for
example, holds US patent 12,386,724 on running a released and an updated
software version side by side and comparing them). What Shadow Lap adds is an
open, QNX-native reference implementation for **brake blending**, with
OS-enforced isolation that is tested route by route, safety checks that are
absolute rather than only relative to v1, and a release-evidence gate that
treats missing evidence as a failure.

> Entry for the QNX Global Hackathon 2026, category **Automotive & Mobility**.

## Why: braking software updates go wrong

In 2025 Volvo recalled about 14,000 US plug-in hybrids and EVs after Brake
Control Module software version 3.5.14, installed at dealers, could lose
braking after coasting downhill for about 100 seconds in "B" mode or One
Pedal Drive (NHTSA recall 25V392). That is a blending failure: regenerative
braking is unavailable, and the braking the driver expects does not arrive.

Shadow Lap's third candidate, **v3**, has a flaw of that kind on purpose: when
the motor is not allowed to take its share (battery nearly full, or the lap's
energy limit used up), it does not make up the difference with the discs. The
`demand_not_met` check catches it.

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

**What is real and what is modelled.** From OpenF1 (unofficial, free for
non-commercial use; about 3.7 samples a second): speed, throttle, brake
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
| v1 (the baseline) | | broke no limit on any frame; peak rear grip use 95% |

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
| v2 stopped from outside 2 s (SIGSTOP) | 0 | 0 | 166 µs | 0.46 ms |
| CPU flood on the shadow side | 0 | 4 | 99 µs | 3.11 ms |
| hostile v2 (7 routes, then burns v1's CPU for 30 s at priority 63) | 0 | 5 | 187 µs | 0.60 ms |

Every scenario: 0 actuator commands rejected, 0 records dropped, 0 clamps.
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
pass exits 1. It writes `manifest.json` beside the summary: lap-file and tool
hashes, seed, ranges, git commit. `python3 tests/test_gate.py` feeds it 18
broken, incomplete and unsafe summaries to prove it fails closed.

GitHub runs all of this on every push (`.github/workflows/shadow-gate.yml`)
and requires v2 and v3 to FAIL and v2.1 to pass, so CI also proves the gate
can reject a bad candidate.

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
| `include/control.h`, `src/control.c` | car limits (battery, motor torque and heat, grip, wings), v1, v2, v2.1 and v3, the absolute limit checks |
| `include/vehicle.h`, `src/vehicle.c` | the simulated car, shared by the live plant and the sweep |
| `src/sweep.c` | `bbw_sweep`: hundreds of laps in varied conditions, in seconds |
| `tools/gate.py`, `.github/workflows/` | release verdict, and GitHub checks on every push |
| `src/plant.c` | simulated car, lap replay, deadline check |
| `src/actuator.c` | QNX resource manager (Linux: FIFO stand-in) |
| `src/v1.c`, `src/v2.c` | the two controllers; v2 has built-in fault injection |
| `src/comparer.c` | joins v1/v2 per frame, writes the evidence |
| `src/hog.c` | CPU burner for the flood test |
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
  interaction, actuator rate limits, sensor faults, wet tracks.
- **Open-loop by nature.** In shadow mode v2 sees the car v1 drives; the
  counterfactual sweep is a first answer to that, using the same model.
- **The deadline-miss fallback is illustrative**: discs only, capped at 95% of
  rear grip. A real car's degraded mode comes from its own hazard analysis.
- **QNX SDP 8.0 in QEMU, not QNX OS for Safety on real hardware.** Nothing
  here is ISO 26262 certified. Verified in QEMU: the actuator resource
  manager, real-time priorities, CPU pinning, v2 as another user. Not yet
  run: the security-policy steps and real hardware (Raspberry Pi).
- **Priority band, not a CPU budget.** v2 can still use up to 63 and move onto
  v1's CPU. v1 and the car out-rank it, but lower-priority system services on
  that core could be slowed. A CPU budget for the shadow (QNX adaptive
  partitioning) is the stronger next step.
- **Shadow mode is not new.** The contribution is the QNX implementation, the
  isolation tests, the absolute checks and the fail-closed gate.
- **Data licence.** OpenF1 is unofficial and for non-commercial use; the lap
  data is downloaded by the user and not stored in this repo. F1 and related
  marks belong to Formula One Licensing B.V.
- The AI analyst only explains evidence; it never decides a verdict. Its
  number checker is tested; a review from a real model is not yet in the repo.

## License

MIT. See [LICENSE](LICENSE).
