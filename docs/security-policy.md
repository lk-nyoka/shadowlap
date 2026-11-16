# Enforcing "v2 cannot touch the brakes" with a QNX security policy

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
- rules for `bbw_v2_t`: it should only need its shared-memory region
  (`/dev/shmem/bbw_shadow`) and nothing belonging to `bbw_plant_t`

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

Pass: `[v2] ATTACK BLOCKED`, a matching entry in the errors log, and v1 still
braking normally. Record the output; it goes in the demo video.
