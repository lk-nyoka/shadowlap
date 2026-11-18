# Testing Shadow Lap, again and again

There are two loops: one anyone can run on Linux or WSL, and one on QNX. The
QNX loop is the one whose timing counts.

## 1. Everything, on Linux or WSL: `scripts/check_all.sh`

```bash
make check                                    # 3 rounds
ROUNDS=10 sh scripts/check_all.sh             # 10 rounds
CLONE=1 ROUNDS=1 sh scripts/check_all.sh      # from a fresh copy of the GitHub repo
sudo -E env "PATH=$PATH" sh scripts/check_all.sh   # adds the attack scenario (needs root)
```

**Once:**
- build from clean, then the C unit tests and the gate tests;
- the safety monitor matches its pinned hash;
- the LICENSE is there, and the README names the QNX version, target and toolchain;
- tamper tests: the same evidence passes with the real pin and is `INVALID` with a wrong one, and an empty evidence file is `INVALID`.

**Every round** (seed = round number, so each round draws new conditions):
- 600 laps each of v2, v3 and v2.1. v2 and v3 must `FAIL`, v2.1 must `PASS_WITHIN_SCOPE`, and v1 must break no limit;
- round 1 also checks that seed 1 gives exactly the README's numbers (600, 304, 0, 13.8 km/h), and the same numbers twice in a row;
- live scenarios: shadow, crash, freeze, sigstop, flood, plus attack as root. In each one:
  - the evidence is valid;
  - v1 breaks no limit;
  - the brakes accepted every v1 command;
  - when the candidate crashes, hangs or is stopped, its missing answers are counted;
  - in the attack, no route is open and the guard stops the CPU burn;
- the HTML report builds.

The result is written to `out/check/check_report.md`. The script exits non-zero if anything failed.

On WSL or a cloud VM, a rare v1 deadline miss shows as a **warning**, because those hosts pause the whole machine now and then. `STRICT_TIMING=1` turns those warnings into failures.

## 2. Timing, on QNX: `repeat.sh`

Start QNX and set its IP (`ifconfig vtnet0 172.31.1.10 netmask 255.255.255.0`). Then, in Ubuntu:

```bash
source ~/qnx800/qnxsdp-env.sh && make TARGET=qnx bundle
scp -r build/qnx-x86_64/bundle/* root@172.31.1.10:/data/shadowlap/
```

In the QNX `#` window:

```sh
cd /data/shadowlap
V2_USER=qnxuser PROD_CPU=0 SHADOW_CPU=1 sh repeat.sh 5 "shadow flood attack" out/repeat
V2_USER=qnxuser PROD_CPU=0 SHADOW_CPU=1 sh repeat.sh 3 "baseline crash freeze sigstop" out/repeat2
```

Back in Ubuntu, copy the results back and summarise them:

```bash
scp -r root@172.31.1.10:/data/shadowlap/out/repeat out/qnx-repeat
scp -r root@172.31.1.10:/data/shadowlap/out/repeat2 out/qnx-repeat2
python3 tools/timing_table.py out/qnx-repeat
python3 tools/timing_table.py out/qnx-repeat2
```

**Pass means:**
- **v1 misses: 0** in every row;
- in the attack runs: no `ATTACK NOT BLOCKED` line, and a `containment: ... ms` line from the guard;
- host stalls can vary. They are the Windows host pausing the VM, not a failure.

If a v1 miss shows up, re-run that scenario before believing it. A miss that comes with hundreds of host stalls in the same run is the host. A miss with 0 stalls is a real finding: keep the folder and look at it.

## 3. The judge's path, end to end

Once before submitting, do exactly what a judge would:

1. In a new folder: `git clone https://github.com/lk-nyoka/shadowlap && cd shadowlap`
2. Follow the README "Run it" section word for word, without using anything from your own folder.
3. `ROUNDS=1 sh scripts/check_all.sh`, which must end with `0 failed`.
4. `python3 tools/live.py` and open http://localhost:8000. Start a lap with v2 on a hot day; it must show `FAIL (this lap)`.

Write down any step that needed something the README didn't say, and fix the README.
