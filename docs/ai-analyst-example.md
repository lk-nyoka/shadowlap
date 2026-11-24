# AI analyst: a real review of a QNX run

Run: the recorded QNX run shown in the demo video and on the replay page (QNX SDP 8.0 in QEMU,
November 2026; candidate v2, hot day + worn tyres, full Monza lap, 7,933 frames).
Model: Qwen2.5-3B-Instruct (Apache-2.0), Q4_K_M GGUF, served locally on a CPU by `llama-cpp-python`.
Command: `python3 tools/analyst.py <run> --provider openai --url http://127.0.0.1:8099/v1 --model qwen2.5-3b-instruct`
Generated: 23 November 2026. Rule-based evidence: 19 episodes. **Unverified numbers: 0, unknown episode ids: 0.**

The review below is unedited model output.

---

## Verdict
v2 is not ready to control the car. The main reason is that v2's decisions led to multiple episodes of overcharging and rear lock risk, which could compromise vehicle safety.

## Problems found
- **Overcharging**: In episode E2 (t_start_s: 8.7), v2 charged at full power until battery reached 90%, leading to an overcharge condition.
- **Rear Lock Risk**: Episodes E3 (t_start_s: 8.8) and E4 (t_start_s: 9.1) show rear lock risk with v2's decisions, indicating a design flaw in handling braking dynamics.

## Fix before the next test
1. In episode E2, reduce v2’s charging power to taper off at 80% battery level.
2. For episodes E3 and E4, implement a check for rear grip safety during braking initiation to avoid locking conditions.

## Tests to run next
- **High-speed braking with full throttle**: Test in lap 1 (episode E1) where v2’s charging power is high but the car is still under control.
- **Braking at low battery levels**: Run episode E5 and E6, focusing on how v2 handles low battery conditions without overcharging.

---
*Written by qwen2.5-3b-instruct from rule-based evidence; checked automatically.*
*Episodes cited: E1, E2, E3, E4, E5, E6.*
*Every number above appears in the evidence.*

---

## Our notes on it

- **Right:** the main finding. v2's built-in flaw is that it keeps charging at full power until the
  battery is 90% full, while v1 starts tapering at 80% (`src/control.c`: `car_accept_frac(soc, 0.90)`
  for v2, `0.80` for v1). The model found that from the evidence alone and proposed the right fix.
- **Right:** the rear-lock risk episodes (E3, E4) that follow at the same braking zone.
- **Weak:** some suggested next tests (for example "braking at low battery levels" for E5/E6) don't
  follow from the evidence. The checker verifies numbers and episode ids, not reasoning, which is why
  the review is advice to an engineer and never changes the verdict or touches the car.
