# Shadow Lap

**Test a new brake-by-wire controller against a real lap without letting it
touch the brakes. Built on QNX 8.0.**

Racing teams get very little track time, and every road-car maker that blends
motor braking with disc braking has the same problem: you can't let untested
control software brake a real car. Shadow Lap runs the new controller (v2)
beside the trusted one (v1). v2 sees every input and makes every decision, but
its decisions only go into a log. v1 stays in charge, and nothing v2 does
(crashing, hanging, hogging the CPU, attacking the brakes) can make v1 late.

The output is evidence: every moment the two disagreed, why, and how serious it was.

> Entry for the QNX Global Hackathon 2026, category **Automotive & Mobility**.
> Working name; rename freely.

## What it looks like

The dashboard replays a **real F1 lap** (Monza 2024 qualifying pole, 1:19.327,
from free public timing data) and shows, moment by moment:

- **the car on a map of the real circuit**, drawn from its real position data,
  with braking zones marked and dots where v2 got it wrong;
- **the rear brakes right now**: what the driver asks for, how v1 (in charge)
  splits it between motor and discs, how v2 (in shadow) would have split it,
  against the tyre-grip limit;
- **one plain sentence** saying what is happening, e.g. *"Braking from 331 km/h
  at 5.1 g. v2 would ask the rear tyres for more than they can grip: the rear
  wheels could lock. v1 stays under the limit."*

Pick the **conditions** at the top (normal day, worn tyres, hot day, or
both). On a normal day v2's flaws stay hidden; with hot weather and worn tyres
the same lap shows v2 risking a rear lock at the first braking zone (about 9 s
in) and over-asking a hot motor later in the lap, while v1 stays safe.

What is real: speed, throttle, brake on/off, track position, lap time. What
is modelled: battery, motor, wings, grip and forces, with grip and downforce
fitted so the real pole lap uses up to 93% of the modelled grip at its hardest
braking (5.6 g).

**Across 600 laps of that real lap** in changing conditions (battery 50–92%,
air 10–45 °C, track grip 85–105%), run in under a second by `bbw_sweep`:

| Finding | Depends on | Result |
|---|---|---|
| v2 risks locking the rear wheels | track grip | 100% of laps at 85–92% grip, 93% at 92–99%, 0% at normal grip |
| v2 over-asks a hot motor | air temperature | 37% of laps at 10–25 °C, 67% at 25–35 °C, 96% at 35–45 °C |
| v2 over-asks a hot motor | lap of the run | 27% on lap 1, 84% on lap 2, 82% on lap 3 |
| v2 overall | | unsafe on 600 of 600 laps: **rejected** |
| v2.1 (flaws fixed) | | unsafe on 0 of 600 laps: **approved** |

### Results on QNX 8.0

Full laps, 9,016 frames each, QNX 8.0 in QEMU (2 CPUs) on a Windows 11 laptop.
v1 and the car pinned to CPU 0, v2 to CPU 1, v2 running as an unprivileged
user (`V2_USER=qnxuser`).

A **v1 miss** means v1 had its full 8 ms after receiving a frame and still
didn't answer. A **host stall** means the simulated car itself was woken more
than 2 ms late (QNX here runs inside a VM on Windows, which still pauses it).

| Scenario | v1 misses | Host stalls | v1 p99 | v1 worst |
|---|---|---|---|---|
| baseline (no shadow) | 0 | 58 | 237 µs | 1.13 ms |
| shadow | 0 | 48 | 597 µs | 1.96 ms |
| v2 crashes | 0 | 26 | 347 µs | 1.77 ms |
| v2 hangs 2 s | 0 | 53 | 615 µs | 2.10 ms |
| v2 SIGSTOPped 2 s | 0 | 15 | 544 µs | 1.23 ms |
| CPU flood on shadow side | 0 | 13 | 185 µs | 0.88 ms |
| v2 attacks the actuator | 0 | 7 | 246 µs | 1.56 ms |

**The attack is blocked by the OS:**
`[v2] ATTACK BLOCKED: opening /dev/bbw/actuator failed: Permission denied`

The first shadow run gave 2 v1 misses with 337 host stalls, far more stalls
than any other run; a rerun gave the row above. We read the first as the
Windows host being busy, and keep both in the record.

### Same laptop, same code, without QNX

| | Ubuntu in WSL (real-time priority) | QNX 8.0 in QEMU |
|---|---|---|
| v1 deadline misses (shadow lap) | 40 | **0** |
| Host stalls | 82 | 48 |
| v1 worst response | 7.96 ms | **1.96 ms** |
| v2 attacks the brakes | not blocked | **blocked** |

On Linux in a cloud container (no Windows underneath) v1 also stayed at 0–2
misses, but nothing stopped the attack: v2's write displaced v1's command for
one frame (the car fell back to disc-only braking; v2's command was never
applied).

## How it works

Five separate processes. Full plain-English walkthrough:
[docs/how-it-works.md](docs/how-it-works.md).

```
 lap replay ─► bbw_plant (the car, owns the clock and the actuator)
                 │  frame every 10 ms
        ┌────────┴─────────────┐
        ▼                      ▼  copy on a ring that never blocks
     bbw_v1 ──write()──►    bbw_v2 (shadow: watches only)
   /dev/bbw/actuator           │ proposals
   (only v1 can open)          ▼
                         bbw_comparer ─► compare.csv, summary.json ─► report.html
```

QNX features used:

- **Resource manager**: the brake actuator is `/dev/bbw/actuator`. v1's
  `write()` is a QNX message, and the plant thread handling it inherits v1's priority.
- **Process isolation + shared memory**: three separate regions, so v2 can
  only ever see its own.
- **Fixed priorities** (`SCHED_FIFO`) and **CPU pinning** (`ThreadCtl` runmask).
- **Permissions and security policies**: v2 is denied the actuator by the OS.
  See [docs/security-policy.md](docs/security-policy.md).

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

**Many laps and the release gate:**

```bash
build/host/bbw_sweep --runs 200 --laps 3 --out out/sweep                    # candidate v2
build/host/bbw_sweep --runs 200 --laps 3 --candidate v2.1 --out out/sweep-v2.1
python3 tools/gate.py out/sweep/summary.json          # REJECTED, with reasons
python3 tools/gate.py out/sweep-v2.1/summary.json     # APPROVED
python3 tools/report.py out                           # adds the many-laps charts
```

GitHub runs the same checks on every push (`.github/workflows/shadow-gate.yml`)
and posts both verdicts in the job summary. Add `--strict` to make a rejection
fail the build.

Run the live system with the fixed candidate or different conditions:
`CANDIDATE=v2.1 sh scripts/run_demo.sh shadow out` or
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
| `include/bbw.h` | messages, never-blocking ring buffer, shared regions |
| `include/control.h`, `src/control.c` | car limits (battery, motor torque and heat, grip, wings), v1, v2 and v2.1, disagreement rules |
| `include/vehicle.h`, `src/vehicle.c` | the simulated car, shared by the live plant and the sweep |
| `src/sweep.c` | `bbw_sweep`: hundreds of laps in varied conditions, in seconds |
| `tools/gate.py`, `.github/workflows/` | release verdict, and GitHub checks on every push |
| `src/plant.c` | simulated car, lap replay, deadline check |
| `src/actuator.c` | QNX resource manager (Linux: FIFO stand-in) |
| `src/v1.c`, `src/v2.c` | the two controllers; v2 has built-in fault injection |
| `src/comparer.c` | joins v1/v2 per frame, writes the evidence |
| `src/hog.c` | CPU burner for the flood test |
| `scripts/run_demo.sh` | the seven scenarios |
| `tools/` | live dashboard, real-lap import (`fetch_openf1_lap.py`), lap from video (`lapgen.py`), synthetic lap, FastF1 import, HTML report, AI analyst, QNX live link |
| `tests/` | ring buffer stress test, strategy tests |

## Honest limits

- Battery, motor, wings and forces are a small illustrative model (`include/control.h`).
  The lap only provides speed, throttle and a brake on/off flag.
- Shadow mode is open-loop: v2 sees a battery shaped by v1's choices.
- F1 cars use a standard control unit; this is a development and test-rig tool.
  The same approach applies to any EV or hybrid with blended braking.
- Verified on QNX 8.0 (QEMU): the actuator resource manager, real-time
  priorities, CPU pinning and running v2 as another user. Not yet run: the
  security-policy steps (docs/security-policy.md) and real hardware (Raspberry Pi).
- The AI analyst's checker is tested; a review from a real model is not yet in the repo.

## License

MIT. See [LICENSE](LICENSE).
