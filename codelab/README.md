# Codelab: a CPU budget for untrusted code on QNX 8.0, without adaptive partitioning

**Time:** about 45 minutes · **Level:** intermediate C · **You need:** QNX SDP 8.0
(free through QNX Everywhere), a QNX 8.0 target (the QEMU image from `mkqnximage`
is fine), and root on that target.

## What you'll build

On QNX 7.1, the usual way to stop one program from eating another's CPU time was
the **adaptive partitioning scheduler (APS)**. QNX SDP 8.0 doesn't have it: asked on
the QNX community forum, the answer was that APS is not available in 8.0, pointing
to "other means available in SDP 8 (sporadic scheduling, per-thread runtime
clocks)" ([forum thread](https://community.qnx.com/sf/go/projects.core_os/discussion.newcode.topc28434)).

Sporadic scheduling only limits a thread that *chooses* it, so it can't contain
code you don't trust. This codelab uses the other tool, **runtime clocks**, to
build `cpuwatch`: a small privileged program that watches every process of one
user from the outside and stops them if they use more than a CPU budget.

You'll learn to:

1. read any process's CPU time with `clock_getcpuclockid()`;
2. find every process a user owns, through `/proc` and `DCMD_PROC_INFO`;
3. turn samples into a sliding-window budget;
4. put the watchdog in QNX's **privileged priority band**, where the watched
   user can't out-rank it;
5. catch a program that hides its work in a child process;
6. understand what this does *not* guarantee.

The code is two small files in this folder: [`cpuwatch.c`](cpuwatch.c) (the
watchdog, about 180 lines) and [`spin.c`](spin.c) (a misbehaving program to test
it with). It was written for [Shadow Lap](../README.md), where the same design
contains an untrusted brake controller; nothing here depends on Shadow Lap.

---

## Step 0: build and copy

On your development machine (Linux or WSL):

```bash
source ~/qnx800/qnxsdp-env.sh
cd codelab
qcc -Vgcc_ntox86_64 -O2 -Wall -o cpuwatch cpuwatch.c
qcc -Vgcc_ntox86_64 -O2 -Wall -o spin spin.c
scp cpuwatch spin root@<target-ip>:/tmp/
```

(For a Raspberry Pi 4 target use `-Vgcc_ntoaarch64le`.)

You need an ordinary, unprivileged user on the target to play the "untrusted"
role. The QEMU image made by `mkqnximage` has one called `qnxuser`; on another
image, add one.

## Step 1: read another process's CPU time

Every process has a CPU-time clock. `clock_getcpuclockid()` gives you its id;
`clock_gettime()` reads it, in nanoseconds of CPU used since the process started:

```c
static int64_t cpu_ns(int pid) {
    clockid_t cid;
    struct timespec ts;
    if (clock_getcpuclockid((pid_t)pid, &cid) != 0) return -1;
    if (clock_gettime(cid, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
```

Read it twice, 50 ms apart: the difference divided by 50 ms is the share of one
CPU the process used in that time. A process spinning flat out shows close to 100%.

## Step 2: find every process a user owns

Watching one process ID is not enough (see step 5). Instead, list `/proc` and ask
who owns each process. On QNX, open `/proc/<pid>/as` and ask the process manager
with `devctl(DCMD_PROC_INFO)`; the answer includes the effective user ID:

```c
#ifdef __QNX__
    snprintf(path, sizeof path, "/proc/%d/as", pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        procfs_info info;                       /* <sys/procfs.h> */
        int rc = devctl(fd, DCMD_PROC_INFO, &info, sizeof info, NULL);
        close(fd);
        if (rc == EOK) return (long)info.euid;
    }
#endif
```

On Linux the same program falls back to the owner of the `/proc/<pid>` directory,
so you can try the logic on your PC first.

## Step 3: a sliding-window budget

`sample()` lists the user's processes and adds up the CPU time each used **since
the last sample**. A process seen for the first time counts from its birth, so a
brand-new child can't slip through between two samples. Keep the running total
in a ring of the last `window / period` samples (20 samples × 50 ms = 1 s) and
compare:

```c
double pct = 100.0 * (total - used[oldest]) / (now - at[oldest]);
if (window_full && pct > budget) { stop_all(uid); ... }
```

Two design choices to notice:

- **It only acts on a full window.** A short burst (loading a file, one heavy
  frame) is allowed; using more than the budget *on average over a second* is not.
- **Stopping is SIGKILL, repeated.** `stop_all()` kills every process it found,
  waits 5 ms, lists the user again and repeats, in case one forked meanwhile.

## Step 4: run above the code you're watching

A watchdog that can be starved is useless. On QNX 8.0, unprivileged threads can use
priorities 1 to 63 by default; only threads with the `PROCMGR_AID_PRIORITY`
ability may go above 63 ([QNX 8.0 Programmer's Guide: Priority range](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/overview_Priority_range.html)).
So `cpuwatch` runs at **64**, as root:

```c
struct sched_param sp = { .sched_priority = 64 };
pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
```

Whatever the watched user does with its own priorities, it can't get above 63,
so it can't out-rank the watchdog. Anything more important than the watchdog (in
Shadow Lap: the trusted controller at 65 and the car model at 70) goes above it.

## Step 5: try it

On the target, as root, start the watchdog for `qnxuser`:

```sh
/tmp/cpuwatch --user qnxuser --budget 40 &
```

Then run the spinner as `qnxuser`. First the simple case:

```sh
on -u qnxuser /tmp/spin 10
```

Then the sneaky case, where the parent sits idle and a child does the burning:

```sh
on -u qnxuser /tmp/spin 10 --child
```

Real output on **QNX 8.0 in QEMU** (24 November 2026). The three commands were
pasted together, so the watchdog and the first `spin` started at the same moment.
The QNX console mixed the lines together; here they are in the order they printed:

```
# /tmp/cpuwatch --user qnxuser --budget 40 &
cpuwatch: user qnxuser: budget 40% of one CPU over 1000 ms, checked every 50 ms
# on -u qnxuser /tmp/spin 10
spin: pid 487442, burning a CPU for 10 s
Killed
cpuwatch: OVER BUDGET: 1 process(es) of qnxuser used 98% of a CPU -> stopped (SIGKILL) after running hot for 975 ms
# on -u qnxuser /tmp/spin 10 --child
spin: pid 499730, burning a CPU for 10 s in a child process
spin: child 507924 spins, parent 499730 stays idle
Killed
cpuwatch: OVER BUDGET: 2 process(es) of qnxuser used 45% of a CPU -> stopped (SIGKILL) after running hot for 467 ms
```

Both 10-second programs were stopped in under a second. The last line is the point
of step 2: **2 processes**. A watchdog that only watched the parent's process ID
would have seen an idle parent and done nothing while the child burned a whole CPU.
(We tried it: 4 seconds, nothing caught.)

Why 975 ms the first time and 467 ms the second? The budget is an *average over the
last second*, and `cpuwatch` only acts on a full window, so a short burst is allowed.

- **First run:** the watchdog and `spin` started together, so the window had to
  fill first. By then `spin` had used 98% of that second. That is the same as in
  Shadow Lap, where the guard starts with the program and stopped a hostile
  program after **962–996 ms in 5 of 5 runs** on QNX 8.0.
- **Second run:** the window had already partly filled with idle time before the
  child started. A program spinning at 100% pushes the average over 40% once it
  has burned about 0.4 s inside the window, plus up to one 50 ms check. Here that
  took 467 ms.

On Linux the same code caught one process after 482 ms and two after 428 ms, with
the window already full.

## Step 6: know what this is, and isn't

- **It's a watchdog, not a reservation.** The watched code *does* run hot for up
  to about one window before it's stopped. What protects your important threads
  in the meantime is priority (step 4), not the watchdog. If they must never be
  delayed at all, they need to out-rank the untrusted code, as in step 4.
- **Exec counts as a new process on QNX.** If you also limit the user's processes
  with `RLIMIT_NPROC = 0`, a launcher that drops privileges and then `exec`s the
  untrusted program fails on QNX with `EAGAIN` (on Linux, exec reuses the
  process, so it works). We hit this ourselves; it's why the watchdog counts
  every process of the user instead of trying to forbid children.
- **Process-count floods:** `cpuwatch` tracks up to 256 processes per user. A
  real deployment should also cap how many processes the user can create.
- **It needs root** (or the abilities to read other users' `/proc` entries and
  signal them).

## Going further

- **Abilities instead of root.** QNX lets you grant a process exactly the
  abilities it needs, for example a privileged priority, without full root:
  look at `procmgr_ability()` and security policies.
- **Forbidding fork natively.** QNX also has a `PROCMGR_AID_FORK` ability
  ("create a new process by calling fork()"). Denying it to the untrusted user
  may be a cleaner way to stop child processes than `RLIMIT_NPROC`. We haven't
  tested this; try it and report back.
- **Per-thread clocks.** `pthread_getcpuclockid()` gives you the same measurement
  per thread, if you need to budget one thread inside a process.

## The real thing

Shadow Lap's version is [`src/guard.c`](../src/guard.c), started by
[`scripts/run_demo.sh`](../scripts/run_demo.sh) next to the untrusted
brake controller. Its recorded QNX runs are in the main [README](../README.md).
