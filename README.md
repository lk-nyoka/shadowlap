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

A full lap of Track A (a synthetic lap shaped like a fast, low-downforce
circuit). v2 is designed to harvest more energy. The report finds it:

- locks the rear wheels in the two hardest braking zones,
- overcharges a nearly full battery in three zones,
- keeps harvesting after the lap's energy limit is used up (a planted bug).

…while v1 keeps braking on time.

### Results so far (not QNX yet)

Full laps, 9,016 frames each. A **v1 miss** means v1 had its full 8 ms after
receiving a frame and still didn't answer. A **host stall** means the
operating system woke the simulated car more than 2 ms late.

**Linux (cloud container):**

| Scenario | v1 misses | Host stalls | v1 p99 latency | v2 gaps |
|---|---|---|---|---|
| baseline (no shadow) | 0 | 32 | 250 µs | n/a |
| shadow | 0 | 20 | 267 µs | 0 |
| v2 crashes | 2 | 5 | 219 µs | 8,128 |
| v2 hangs 2 s | 0 | 8 | 263 µs | 100 |
| v2 SIGSTOPped 2 s | 0 | 9 | 247 µs | 99 |
| CPU flood on shadow side | 1 | 13 | 124 µs | 0 |
| v2 attacks the actuator | 1 | 38 | 265 µs | n/a |

The 2 misses in the crash run happened 21 s after the crash, back to back,
so they are host jitter, not caused by v2. On Linux the attack is **not**
blocked: v2's write displaced v1's command for one frame (the plant fell back
to disc-only braking; v2's command was never applied). Stopping that is the
job of QNX permissions and security policy.

**Windows 11 laptop (Ubuntu in WSL), one shadow lap with real-time priority:**
40 v1 misses and 82 host stalls, in bursts. Windows pauses the whole Ubuntu
virtual machine, and real-time priority inside a VM can't prevent that.

**QNX:** next. These are the numbers the submission should quote.

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

**Watch it live:** in a second terminal run `python3 tools/live.py out`, then
open http://localhost:8000 while the demo runs. It follows each scenario as it
starts. For a run on QNX, stream it to your PC with
`sh tools/follow_qnx.sh <qnx-ip> shadow` and watch `python3 tools/live.py out-qnx`.

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

Use a real lap instead of the synthetic one (FastF1 may now need a free F1TV
login; not tested yet, see the note at the top of the script):

```bash
pip install fastf1
python3 tools/fetch_fastf1_lap.py --year 2024 --event Monza --session Q --out data/lap_real.csv
LAP=data/lap_real.csv sh scripts/run_demo.sh shadow out
```

## Repo map

| Path | What |
|---|---|
| `include/bbw.h` | messages, never-blocking ring buffer, shared regions |
| `include/control.h`, `src/control.c` | car limits, v1 and v2 strategies, disagreement rules |
| `src/plant.c` | simulated car, lap replay, deadline check |
| `src/actuator.c` | QNX resource manager (Linux: FIFO stand-in) |
| `src/v1.c`, `src/v2.c` | the two controllers; v2 has built-in fault injection |
| `src/comparer.c` | joins v1/v2 per frame, writes the evidence |
| `src/hog.c` | CPU burner for the flood test |
| `scripts/run_demo.sh` | the seven scenarios |
| `tools/` | synthetic lap, FastF1 import, HTML report, live dashboard, AI analyst, QNX live link |
| `tests/` | ring buffer stress test, strategy tests |

## Honest limits

- Battery, motor and forces are a small illustrative model (`include/control.h`).
  The lap only provides speed, throttle and a brake on/off flag.
- Shadow mode is open-loop: v2 sees a battery shaped by v1's choices.
- F1 cars use a standard control unit; this is a development and test-rig tool.
  The same approach applies to any EV or hybrid with blended braking.
- Not yet verified on QNX: the resource manager, CPU pinning, `on -u`, and the
  security-policy steps. Their code and steps follow QNX 8.0 documentation.
- The AI analyst's checker is tested; a review from a real model is not yet in the repo.

## License

MIT. See [LICENSE](LICENSE).
