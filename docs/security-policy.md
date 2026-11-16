# Enforcing "v2 cannot touch the brakes" with a QNX security policy

Current state (16 November). First QNX run of the seven-route attack, v2 as
`qnxuser`: five routes blocked (actuator, v1's region, evidence, writing its
own inputs, signalling v1); two open (raising its priority above v1's 45, and
moving onto v1's CPU). Fix applied: on QNX the car runs at 70 and v1 at 65,
in the privileged band (64 and up need `PROCMGR_AID_PRIORITY`; unprivileged
processes reach at most 63), so v2 can no longer out-rank them. Re-run on
QNX: six routes blocked, v2 reaches at most priority 63, and with v2 burning
v1's CPU at 63 for 30 s v1 had 0 deadline misses in 8,914 frames.

CPU budget: QNX SDP 8.0 discontinued adaptive partitioning. `bbw_guard`
budgets the candidate instead by reading its CPU-time clock from outside, at
priority 64, and stopping it when it goes over. A security policy that denies
the shadow type abilities it does not need is the next layer.

File permissions already keep v2 out when it runs as another user
(`V2_USER=qnxuser`). A QNX security policy is stronger: it works by
*security type*, so it applies even if v2 runs as root.

QNX's documented workflow is to let the system *record* what each process
does, then lock the system down to exactly that. Our v2 never opens the
actuator in normal operation, so a recorded policy naturally gives v2 no right
to it. The attack test then proves the lock works.

Status: **not yet run.** The commands below come from QNX 8.0's documented
`mkqnximage --secpol` / `secpolgenerate` workflow. Confirm every flag with
`--help` on your install, and update this file with what actually happened.

## 1. Boot an image in policy-recording mode

```bash
mkdir -p ~/qnxvm-secpol && cd ~/qnxvm-secpol
mkqnximage --type=qemu --secpol=open --run
```

## 2. Run each program with its own security type

On QNX, `on -T <type>` starts a program with a security type. Edit
`run_demo.sh` to start the programs like this (or run them by hand):

```sh
on -T bbw_plant_t    ./bbw_plant --lap data/lap_a.csv &
on -T bbw_v1_t       ./bbw_v1 &
on -T bbw_comparer_t ./bbw_comparer --out out/secpol &
on -T bbw_v2_t       ./bbw_v2 &
```

Run a full lap so every normal action gets recorded. Do **not** run the
attack scenario while recording, or the attack becomes "allowed".

Check the types took effect:

```sh
pidin -f an_
```

## 3. Pull and read the recorded policy

```bash
sftp root@<QNX-IP>
get /dev/secpolgenerate/policy policy.txt
```

Use `sftp`, not `scp`: the file is generated as it is read.

What to look for in `policy.txt`:

- rules for `bbw_v1_t` that allow it to reach the plant's actuator
  (`/dev/bbw/actuator`)
- rules for `bbw_v2_t`: it should only need to read `/dev/shmem/bbw_shin`
  and write `/dev/shmem/bbw_shout`, and nothing else belonging to
  `bbw_plant_t` (no actuator, no `/bbw_prod`, no `/bbw_rec`)

If anything lets `bbw_v2_t` reach the plant or the actuator, delete it.

## 4. Enforce it

Quick route on the running system: add `option restricted` to
`/data/var/secpol/config`, check it with
`secpolgenerate -c /data/var/secpol/config -N`, then reboot.

Release route: rebuild with the policy baked in:

```bash
secpolcompile -o secpol.bin policy.txt      # proves the policy compiles
mkqnximage --type=qemu --secpol=secure --policy=policy.txt --run
```

## 5. The test that matters

```sh
on -T bbw_plant_t ./bbw_plant &
on -T bbw_v1_t    ./bbw_v1 &
on -T bbw_v2_t    ./bbw_v2 --attack          # run as root on purpose
cat /dev/secpolgenerate/errors               # restricted mode: the denial is logged here
```

Pass: every route reports `ATTACK BLOCKED` (the attack now tries seven:
actuator, v1's region, evidence, its own inputs, signalling v1, priority,
CPU), matching entries in the errors log, and v1 still braking normally.
Record the output; it goes in the demo video.
