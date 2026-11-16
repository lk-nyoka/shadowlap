# How it works, in plain words

## The problem

A racing team (or any EV maker) has a brake controller that works. Engineers
write a better one. How do you find out whether the new one is actually
better without letting it brake a real car?

Shadow mode: the new controller runs alongside the trusted one, sees exactly
the same inputs, makes its own decisions, and those decisions go nowhere
except into a log. Afterwards you compare.

## What is being controlled

When the driver brakes, the rear of the car slows in two ways at once:

- **the motor** running backwards, which recharges the battery (harvesting)
- **the brake discs**, which turn the energy into heat

Every 10 ms a controller decides the split. Harvest too little and you waste
energy. Harvest too much and you overcharge the battery, break the lap's
energy limit, or ask the rear tyres for more grip than they have.

- **v1** is the trusted controller. It is in charge.
- **v2** is the candidate. It tries to harvest more: it moves 6% more braking
  to the rear, charges at full power until the battery is 90% full (v1 eases
  off from 80%), and it has deliberate flaws: it forgets the lap energy limit,
  the tyres' grip limit and the motor's temperature limit.
- **v2.1** is v2 with those flaws fixed, keeping its extra harvesting where it
  stays inside the limits. It shows the end of the story: the evidence found
  the flaws, the fix removes them, and the gate passes it *within the tested
  scope*, ready for the next validation stage.
- **v3** is a different idea (smoother motor/disc blending) with a different
  flaw: when the motor is not allowed to take its share (battery nearly full,
  or the lap's energy limit used up), it does not make up the difference with
  the discs, so the car brakes less than the driver asked. That is the same
  kind of failure as the 2025 Volvo recall, where braking was lost after long
  coasting in strong-regen modes.

### What the car model includes

Grip and downforce are fitted to a real pole lap (Monza 2024 qualifying): the
real lap uses up to 93% of the modelled grip at its hardest braking, 5.6 g.
The other numbers are illustrative.

- **Battery:** accepts less charge as it fills; a per-lap harvest limit.
- **Motor (electrical):** a torque limit at low speed and a power limit above
  it; it heats up with use, cools towards the air, and limits itself above
  130 °C.
- **Aerodynamics:** F1 2026-style wings. In straight-line mode the car has
  less downforce; when braking starts the wings take about 0.2 s to close, so
  grip is lower for that moment.
- **Track grip:** a factor for cold, dusty or green tracks.

## Many laps, not one

The live demo proves timing and isolation. `bbw_sweep` answers the
engineering question: across many laps and conditions, how often would the
candidate be unsafe, and what would it gain? It uses the same car model and
strategies with no real-time clock, so 600 laps take under a second. Hard
limits (rear lock, demand not met) count from one 10 ms frame; soft limits
(battery, lap energy, motor heat) once they last 50 ms. `tools/gate.py` turns
the result into PASS_WITHIN_SCOPE, FAIL or INVALID, and fails closed.

`--mode counterfactual` lets the candidate drive its own copy of the car, so
its own decisions shape the battery, lap energy and motor heat it sees next.

## The programs

| Program | Job | Can it brake the car? |
|---|---|---|
| `bbw_plant` | The simulated car. Replays the lap, owns the clock, owns the brake actuator, tracks the battery. | It *is* the car |
| `bbw_v1` | Trusted controller. Reads each frame, sends its decision to the actuator. | Yes, the only one |
| `bbw_v2` | Shadow controller. Reads copies of the frames, sends decisions to the comparer. | No |
| `bbw_comparer` | Lines up v1's and v2's decisions per frame and labels every disagreement. | No |
| `bbw_hog` | Burns CPU, to show a greedy shadow side cannot starve v1. | No |
| `bbw_sweep` | Runs hundreds of laps offline with the same car and brains, for statistics and the release gate. | No (no real car at all) |

## One cycle (10 ms)

1. The plant builds a **frame**: speed, how hard the driver is braking,
   battery charge, energy harvested so far this lap, rear grip limit.
2. It gives the frame to v1 and **pins a copy on a read-only noticeboard** for v2.
3. v1 decides and writes its command to the actuator.
4. 8 ms after handing v1 the frame, the plant takes v1's command. If none
   arrived, that is a **deadline miss** and the discs take over, capped at 95%
   of the rear grip (an illustrative fallback; a real car's degraded mode
   comes from its hazard analysis). If the plant itself woke up more than 2 ms late (the host
   OS stalled it), that cycle is also counted as a **plant overrun / host
   stall**, so v1 is never blamed for time it did not get.
5. The actuator refuses commands that are not sane numbers (not finite,
   negative, absurdly large) or that go back in time, and never lets a bad
   command replace the last good one. The plant enforces physics anyway,
   updates the battery and writes a record for the comparer.

Meanwhile v2 reads the noticeboard whenever it gets CPU time and posts its own
decision for the comparer.

## Why v1 never waits for v2

The noticeboard is a fixed-size ring in shared memory that v2 maps
**read-only**. The plant writes each frame over the oldest one and moves on;
it never learns whether v2 kept up, so nothing v2 does can reach back to it.
If v2 falls too far behind, it finds the old frames gone and counts them as
lost (each slot carries its own lock, so v2 gets either a whole frame or
"lost", never a half-written one: the stress test reads 2.5 million frames
with 0 torn). v2 writes its answers to a separate region, which the comparer
treats as untrusted: a nonsense value or sequence number is dropped and
counted. v1 does not even know v2 exists.

**Example: v2 freezes for 2 s as the car brakes for the first chicane.**
v1 keeps answering every 10 ms and the car brakes normally. The comparer sees
no answers from v2 for those frames. The comparer waits up to 1 s for each
answer; anything later is marked `shadow_missing` (about 100 frames in the
test run). The report shows that as a gap. It never counts a gap as agreement.
When v2 wakes up it works through the frames still on the noticeboard.

## Keeping v2 away from the brakes

Layers, from weakest to strongest:

1. **By design**: v2's code never opens the actuator.
2. **By file permissions** (QNX and Linux): the production region, the
   evidence and the actuator belong to root and are closed to other users;
   v2's inputs are readable but not writable. Run v2 as another user
   (`V2_USER=qnxuser`) and the OS enforces this.
3. **The actuator checks what it is told**, whoever sends it.
4. **By QNX security policy** (prepared, not yet run): v2 runs with its own
   security type, and the policy gives that type no rights it does not need.
   See [security-policy.md](security-policy.md).

The `attack` scenario tests seven routes and reports each one: open the
actuator, open v1's region, open the evidence, write its own inputs, signal
v1, raise its priority above v1's, move onto v1's CPU. As an unprivileged
user on Linux the first six are blocked. Moving CPUs is allowed (v1 still
out-ranks it there). On QNX, raising the priority may be allowed by default,
which is why a CPU budget for the shadow is on the list.

## What the disagreement labels mean

| Label | Meaning |
|---|---|
Every limit is checked on the candidate's command itself, not only where it
differs from v1, and the same checks run on v1 (a flaw both share is still
caught, and a v1 that breaks a limit makes the evidence INVALID).

| Label | Kind | Meaning |
|---|---|---|
| `agree` | | Inside every limit and within 50 N of v1 |
| `within_limits` | | Different from v1, inside every limit |
| `rear_lock_risk` | hard | Asked the rear tyres for more braking than they can grip |
| `demand_not_met` | hard | Braked the rear less than asked, though grip allowed more |
| `over_battery` | soft | Charged harder than the battery accepts |
| `over_lap_cap` | soft | Kept harvesting after the lap's energy limit was used |
| `over_motor` | soft | Asked a hot motor for more braking than it can safely give |
| `shadow_missing` | | No valid answer for this frame (none, late, or nonsense) |

Hard limits count from a single 10 ms frame. Soft limits are recorded every
frame and count as an episode once they last 50 ms.

## Where QNX matters

- **Separate processes with memory protection**: v2 crashing cannot corrupt v1.
- **The actuator is a QNX resource manager** (`/dev/bbw/actuator`). Every
  `write()` from v1 is a QNX message, and the plant thread that handles it
  inherits v1's priority. On Linux the same code uses a FIFO, and the flood
  test caught a bug there that QNX's priority inheritance hides (see below).
- **Fixed priorities** (`SCHED_FIFO`): plant 50, v1 45, comparer 20, v2 10.
- **CPU pinning** (`ThreadCtl` runmask): production on one core, shadow on
  another. A runmask belongs to one thread, so every safety-relevant thread
  (including the plant's actuator thread) sets and verifies its own.
- **Refusing to run without real time**: with `REQUIRE_RT=1` (default on QNX)
  the plant and v1 exit if they cannot get their priority, CPU and locked
  memory, and the evidence records whether real time was in force.
- **Security policies** (next step): OS-enforced rights by security type.

### A finding from testing

On the first Linux flood run, v1 missed 3,599 of 9,016 deadlines. Cause: the
plant's actuator thread had been started before the plant raised its own
priority, so it ran at normal priority and the shadow-side CPU burners starved
it. Fixed by setting priority first. On QNX, the resource manager thread would
have inherited v1's priority from v1's message anyway.

A second Linux-only effect: Linux pauses *all* real-time threads once
real-time work uses 95% of a second (`sched_rt_runtime_us`). With burners at
real-time priority, that pause hit v1 too (43 misses in 30 s). QNX has strict
priority preemption, so this is a test to run on QNX with `HOG_PRIO=10`.

### What WSL2 showed

On a Windows 11 laptop (Linux in WSL2, which is a Hyper-V virtual machine),
one lap with nothing misbehaving gave 40 v1 deadline misses and 82 host
stalls, even with real-time priority. The misses came in bursts at the same
moments as the stalls: Windows was pausing the whole Linux VM, v1 included.
QNX in QEMU on the same laptop gave 0 misses, but it also runs on top of the
Windows scheduler, so this compares two setups, not two operating systems. A
fair comparison is QNX and PREEMPT_RT Linux on the same real hardware.

## What this is not

- Not real F1 data inside the car. The lap gives speed, throttle, a brake
  on/off flag and position at about 3.7 Hz, interpolated to 100 Hz. Battery,
  motor and forces come from a small model in `include/control.h`. Every "kW"
  and "MJ" in the report is a model output, and the car's speed follows the
  recorded lap whatever the controllers do.
- Not closed-loop in shadow mode. v2 sees a battery shaped by v1's choices;
  the counterfactual sweep lets v2 drive its own copy of the model car.
- Not a safety approval. A pass means "no limit broken in this campaign",
  evidence for hardware-in-the-loop and vehicle tests, not a replacement for
  them or for an ISO 26262 safety case.
- Not code that runs on an F1 car. F1 uses a standard control unit. This is a
  development and test-rig tool, and the same idea applies to any EV or hybrid
  that blends motor and disc braking.
