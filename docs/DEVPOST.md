# Devpost submission: Shadow Lap

Everything below is ready to paste into the Devpost form, field by field.
The video and codelab links are filled in.

---

## Project name

Shadow Lap

## Elevator pitch (200 characters max)

Test a brake software update without putting it in control. On QNX 8.0 the update runs in shadow with no path to the brakes; we attack that wall ourselves, and every run ends in evidence.

## Category

Automotive & Mobility

## Video

https://youtu.be/zPbI2RrV_hg

## Try it out

- https://github.com/lk-nyoka/shadowlap
- https://lk-nyoka.github.io/shadowlap/replay/ (the real dashboard replaying a run recorded on QNX, no install)
- https://github.com/lk-nyoka/shadowlap/blob/main/codelab/README.md (codelab)

## Built with

`c` · `qnx` · `qnx-sdp-8.0` · `qemu` · `posix` · `resource-manager` · `python` · `javascript` · `github-actions` · `openf1` · `claude` · `kokoro-tts`

**AI tools used to build it:** Claude (Anthropic) helped write and review code, tests and docs, and edited the demo video; every claim was checked against runs. The narration voice is Kokoro-82M (Apache-2.0), run locally.

---

## About the project

### Inspiration

**A driver loses braking on a long downhill because a brake-software update shipped with a flaw no test caught.** That happened:  in 2025 Volvo recalled more than 11,000 US plug-in hybrids and EVs (NHTSA recall 25V392). With brake software version 3.5.14, braking could be lost after about 100 seconds of coasting downhill in one-pedal mode.

Racing teams have the same problem. A new brake-blending controller (how much braking the motor takes, and how much goes to the discs) can't be trusted on a real car until it has been tested. But a real car is the only place it meets real conditions.

### What it does

**In one sentence: the update has no path to the brakes, enforced by QNX permissions rather than promises, and we attack that wall to prove it.**

Two pieces of brake software run side by side on QNX:

- **v1** is today's trusted software. It stays in charge of the brakes.
- **v2** is the update being tested (the *candidate*). It runs beside v1 in shadow.


- **v1 keeps sole control of the brakes.** Only v1 can open the brake actuator.
- **The candidate sees exactly the same inputs** and makes every decision. Its decisions only go to a log.
- **A separate safety monitor** checks both controllers against fixed limits every 10 ms: rear tyre grip, battery charge power, motor force, lap energy, and whether the requested braking was actually delivered.
- **Every run produces evidence**: every moment the candidate broke a limit, a plain-language explanation of why, and an estimate of the consequence. One example: "the car would be 6.9 km/h too fast here".
- **A release gate turns the evidence into a verdict**: `PASS_WITHIN_SCOPE`, `FAIL` or `INVALID`. It fails closed: missing, incomplete or tampered evidence is `INVALID`, never a pass. A pass always states its scope: which laps, which conditions, which model.
- **A live dashboard** replays a real qualifying lap at Monza (OpenF1 data) and shows three things: what the candidate is doing, whether v1 is unaffected, and whether the evidence can be trusted.

**Measured on QNX first** (QNX SDP 8.0 in QEMU, 16–18 November, all recorded):

- **Every attack route blocked or contained, 5 runs out of 5.** That includes the update hiding its CPU use in a child process: the guard caught both processes every time, after 962–996 ms.
- **v1 missed 10 of 374,388 deadlines (0.003%) over 42 laps with an update running.** Five of those happened while Windows froze the whole VM for 0.17–0.69 s. We report all of them (details below).
- **The flawed update fails on QNX too**, on the same real lap, while v1 stays inside every limit (video, 0:48).

**Then modelled, at scale.** Three updates show the range. Two have flaws written on purpose; the third is the fix:

| Candidate | What it changes | 600 laps, changing conditions | Verdict |
|---|---|---|---|
| v2 | more rear braking, more harvesting | limit broken on 600 of 600 laps | FAIL |
| v3 | smoother blending, but doesn't hand braking to the discs when the motor can't take it (inspired by the class of failure in the recall above, not a reproduction of it) | 304 of 600 | FAIL |
| v2.1 | v2 with its flaws fixed | 0 of 600 | PASS_WITHIN_SCOPE |

The conditions varied across the 600 laps: battery 50–92%, air 10–45 °C, tyre grip 85–105%. The result holds beyond that one campaign: across five campaigns with different random seeds (3,000 laps), v2.1 broke no limit on any lap, and v3 failed every campaign (301–336 laps of 600). In v3's worst case, the car leaves a braking zone an estimated 13.8 km/h too fast, 6 m further on. This is a model estimate, not a crash test.

### How it uses QNX

Shadow Lap is a set of QNX 8.0 processes. The isolation that keeps the candidate away from the brakes is enforced by QNX, and we tested it route by route with a hostile candidate.

| What | Where in the repo | QNX feature |
|---|---|---|
| Brake actuator as a QNX resource manager, `/dev/bbw/actuator`, mode 0620, owned by the trusted side: the candidate, running as its own unprivileged user, gets "Permission denied" | `src/actuator.c` (`resmgr_attach`, `io_write`, `iofunc_write_verify`, `dispatch_block`) | Resource managers, POSIX permissions on the path namespace |
| Privileged priority band: the car at 70, v1 at 65, the guard at 64. The candidate, an unprivileged user, tops out at 63 whatever it does | `include/bbw.h` (`BBW_PRIO_*`), `src/common.c` | Priorities 64+ need `PROCMGR_AID_PRIORITY` |
| CPU pinning of the car and v1 | `src/common.c` (`ThreadCtl(_NTO_TCTL_RUNMASK, …)`) | Run masks |
| Hostile-candidate test: tries every route to the trusted side and reports each one | `src/v2.c --attack`, `scripts/run_demo.sh attack` | Permissions, abilities, `ThreadCtl` |
| CPU-budget guard. Adaptive partitioning is not in QNX 8.0, so we built an outside watchdog: every 50 ms it reads the CPU-time clock of **every process the candidate's user owns** and stops them all above 40% of a CPU over 1 s, so a child process can't hide the work | `src/guard.c` (`clock_getcpuclockid`, `DCMD_PROC_INFO`, `SIGKILL`) | Per-process CPU-time clocks, `/proc` |
| Trusted launcher: drops the candidate to its own user, sets hard limits (256 MB of memory, 64 open files; on Linux also no new processes), then `exec`s in place | `src/runas.c` | `setuid`/`initgroups`, `setrlimit` |
| Read-only inputs: the candidate maps the input ring read-only; its output goes to a separate region that is validated before use | `src/v2.c`, `src/comparer.c`, `include/bbw.h` | POSIX shared memory, `PROT_READ` |
| Build and deploy | `Makefile` (`make TARGET=qnx bundle`, `qcc -Vgcc_ntox86_64`) | QNX SDP 8.0 toolchain |

**Timing, measured on QNX SDP 8.0 in QEMU.** Every scenario was repeated with `scripts/repeat.sh`:

- **17 November:** 15 laps (shadow, CPU flood, attack; 133,710 frames). v1 missed its 8 ms deadline 0 times; its slowest answer took 4.45 ms.
- **18 November, with the fixed launcher and guard:** 27 laps across all seven scenarios (240,678 frames). v1 missed 10 deadlines (0.004%). We checked every one frame by frame:
  - 5 came while the **whole VM was frozen** for 170–690 ms (the car model was late too): the Windows host;
  - 4 came during short 2–8 ms pauses we can't attribute;
  - 1 was the first frame of a run.

  The laptop was noisier that evening (it was also recording the demo): v1's p99 doubled **even in the baseline runs**, which have no update and no guard.
- On a missed frame the car falls back to discs only for that 10 ms. Real timing claims need real hardware; the README has the full per-scenario tables.

**The attack test found a real weakness, and QNX's design gave us the fix.** In the first QNX run, the candidate could raise its priority above v1's (v1 was then at 45). The fix: on QNX, unprivileged processes can reach at most priority 63, so we moved the car and v1 into the privileged band above it. The same test now reports that route blocked.

All attack routes, as recorded on QNX:

| Route | Result |
|---|---|
| Open the brake actuator | blocked |
| Open v1's memory | blocked |
| Open the evidence | blocked |
| Write its own inputs | blocked |
| Signal v1 | blocked |
| Raise its priority above v1 | blocked |
| Move onto v1's CPU | contained (out-ranked) |
| Burn that CPU | stopped by the guard |
| Allocate and touch 1 GB of memory | blocked (memory limit) |
| Open 4096 files | blocked (open-file limit) |
| Start a child process to hide CPU use | allowed on QNX (there, `exec` counts against the process limit); the guard counts every process of the update's user and stopped **both** in all 5 runs (962–996 ms) |

### The AI part

**We ran it on the recorded QNX run from the video** (v2, hot day, worn tyres), on 23 November, with a free local model (Qwen2.5-3B-Instruct, on a CPU). Its verdict: *"v2 is not ready to control the car."* It found the battery overcharge starting at 8.7 s and the rear-lock risk at 8.8–9.1 s, and proposed making v2 start tapering its charging at 80% battery. That is exactly the flaw we planted (v2 waits until 90%; v1 tapers from 80%). **0 unverified numbers.** Some of its suggested next tests were weak, which is why the review is advice and never the verdict. Full, unedited review: [`docs/ai-analyst-example.md`](https://github.com/lk-nyoka/shadowlap/blob/main/docs/ai-analyst-example.md).

`tools/analyst.py` writes an engineering review of a run: a verdict, the problems it found, fixes to make, and the next tests to run. It runs with any free local model (llama.cpp as we did, or Ollama) or a free Gemini key.

It is built so the AI cannot make things up unnoticed:

- Fixed rules find the evidence first.
- The model only sees that evidence and must cite episode IDs for every claim.
- Every number it writes is checked against the evidence. Anything unverified is listed at the bottom.
- It never touches the car: it runs after the lap, on the log.

### How we built it

- **C (POSIX + QNX APIs)** for the processes: car model, v1, candidate, comparer, actuator, guard and launcher.
- **Python** for the gate, report, AI analyst and dashboard. The dashboard is a single file with no web framework.
- **Real lap data** from OpenF1, credited and used under CC BY-NC-SA 4.0. Its timing and positions are sampled about 3.7 times a second and interpolated to 100 frames a second.
- **Testing:**
  - unit tests;
  - a gate test suite;
  - GitHub Actions running the gate on every push;
  - `scripts/repeat.sh` for repeated QNX timing runs;
  - `scripts/check_all.sh`, which checks the whole project in rounds, with a new random seed each round.
- **Two rounds of outside design review.** Each review claim was checked against the code. The fixes included:
  - the fail-closed gate;
  - read-only candidate inputs;
  - absolute limit checks that also apply to v1;
  - the privileged priority band;
  - the guard;
  - the launcher limits.

### Challenges

- **Adaptive partitioning isn't in QNX 8.0.** We had planned to give the candidate a hard CPU budget with it. We replaced it with an outside guard and measured how long it takes to act (968–969 ms) instead of claiming it's instant.
- **The guard was watching the wrong process.** QNX's `on -u` starts a child process, so the guard watched the launcher, not the candidate. We wrote `bbw_runas`, which `exec`s in place so the process ID stays the same.
- **A limit that worked on Linux broke the candidate on QNX, and our evidence didn't notice.** On QNX, `exec` counts against the "no new processes" limit, so the candidate never started in 27 runs, and those runs still looked valid. We fixed both: the guard now watches every process of the candidate's user (so a child process can't hide CPU use), and a run where the candidate never answers is now `INVALID`.
- **Telling a timing failure apart from the host pausing the VM.** QNX runs in QEMU inside WSL2 on a Windows laptop. We separated "v1 missed a deadline" from "the host stalled the whole VM" and report both. In 27 runs with no candidate at all, v1 missed 4 deadlines in 240,678 frames, each one while the car model itself was frozen 10–375 ms by the host. That is the floor of this setup, not QNX.

### Accomplishments we're proud of

- A hostile-candidate test that **found a real weakness on QNX**, and a fix that uses QNX's own privilege model.
- A verdict that **can't pass by accident**: missing evidence is `INVALID`, and the safety checks' code is pinned by hash.
- Every v1 deadline miss is counted and explained, not hidden: 10 in 374,388 frames on QNX, half of them during host freezes, while the update crashed, hung, was starved and attacked.
- A launcher bug that silently stopped the update from starting on QNX was caught, and the evidence now fails closed on it (`INVALID: the candidate never answered`).

### What we learned

- On QNX, priority is a permission, not just a number.
- Shadow testing is only as good as its isolation. The isolation has to be attacked, not assumed.
- A "pass" needs its scope written next to it, or people read it as a safety certificate.

### What's next

- Run on real QNX hardware (Raspberry Pi 4/5 first; the build already supports `QNX_ARCH=aarch64le`).
- Feed it live data from a test vehicle, not a replayed lap.
- Add more candidates and limits, such as ABS interaction and front-axle blending.
- Compare against a hardware-in-the-loop bench. It is meant to sit alongside those, not replace them.

### Honest limits

- QNX SDP 8.0 runs in QEMU, nested inside WSL2 on a Windows laptop. It is not an automotive ECU and not QNX OS for Safety.
- The car's forces, battery, motor and grip are modelled.
- The replayed speed doesn't change with the candidate's braking. Extra-speed figures are model estimates.
- The guard is a watchdog, not a CPU reservation.
- Nothing here is ISO 26262 certified. `PASS_WITHIN_SCOPE` is not proof of vehicle safety.
- **Shadow mode itself is not new.** Comparing old and new vehicle software side by side is described in patents (Bosch US 12,386,724; Hitachi Astemo US 12,536,094). What Shadow Lap offers is the combination, as an open, QNX-based reference implementation for brake blending:
  - isolation enforced by QNX and attacked route by route;
  - a fail-closed evidence gate;
  - plain-language explanations of every moment.
- No car maker, racing team or QNX has endorsed this project.

### Built during the hackathon

The repository was created on 16 November 2026, after the hackathon opened on 2 November. Every commit is in its history. Everything in it was built during the submission period.

---

## Testing instructions (for judges)

No special hardware is needed. Two paths:

**A. Any Linux or WSL2 machine (about 5 minutes):**

```bash
git clone https://github.com/lk-nyoka/shadowlap && cd shadowlap
make && make test
python3 -m venv .venv && .venv/bin/pip install numpy matplotlib
ROUNDS=1 sh scripts/check_all.sh          # builds, tests, 600-lap sweeps, live scenarios, report
.venv/bin/python tools/live.py            # dashboard on http://localhost:8000
```

**B. QNX SDP 8.0 in QEMU** (the README section "Run it" has the full steps, and `docs/setup-windows11.md` covers Windows 11):

```bash
source ~/qnx800/qnxsdp-env.sh && make TARGET=qnx bundle
# copy build/qnx-x86_64/bundle to the target, then on QNX:
V2_USER=qnxuser PROD_CPU=0 SHADOW_CPU=1 sh run_demo.sh all out
```

## Codelab / tutorial (bonus)

https://github.com/lk-nyoka/shadowlap/blob/main/codelab/README.md

**A CPU budget for untrusted code on QNX 8.0, without adaptive partitioning.** About 45 minutes. It builds `cpuwatch`, a privileged watchdog that reads every process of one user through runtime clocks (`clock_getcpuclockid`) and `DCMD_PROC_INFO`, enforces a sliding-window budget from QNX's privileged priority band, and catches a program that hides its work in a child process. It also covers what a watchdog does not guarantee, and the QNX-specific trap we hit (`exec` counts against `RLIMIT_NPROC`).

---

## Before you press Submit

- [ ] YouTube video **Public** (the rules require publicly visible; unlisted is not enough), under 3:00, **shows it running on QNX**, no third-party logos or music, captions uploaded (`shadow-lap-final.srt`)
- [ ] Repo public; MIT licence shows in the GitHub About box
- [ ] GitHub About: description + topics (`qnx`, `automotive`, `brake-by-wire`, `shadow-mode`, `real-time`)
- [ ] Fresh-clone test passed: `CLONE=1 ROUNDS=1 sh scripts/check_all.sh`
- [ ] Category: Automotive & Mobility (only one)
- [ ] Your private email isn't in any public file
- [ ] Optional bonus: codelab/tutorial URL (separate $3,500 prize; any QNX 8.0 topic)
- [x] README video link filled in
- [ ] After submitting: save Devpost's confirmation email; if anything fails, email support with the full submission
