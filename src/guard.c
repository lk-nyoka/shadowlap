/*
 * bbw_guard - an outside CPU budget for the shadow candidate.
 *
 * QNX 8.0 has no adaptive partitioning (discontinued in SDP 8.0); QNX points
 * to sporadic scheduling and per-thread/process runtime clocks instead.
 * Sporadic scheduling only limits a thread that chooses it, so a hostile
 * candidate could switch it off. This guard uses the other tool: it reads the
 * candidate's CPU-time clock from OUTSIDE, every 50 ms, and if the candidate
 * used more than its budget over the last second it is stopped (SIGKILL) and
 * the event is logged. The guard runs at a privileged priority (QNX: 64, just
 * above anything an unprivileged process can reach, below v1 and the car), so
 * the candidate cannot starve it.
 *
 *   bbw_guard --pid P [--budget-pct 40] [--window-ms 1000] [--period-ms 50] [--cpu C]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "bbw.h"

#define MAXS 256

int main(int argc, char **argv) {
    int pid    = bbw_arg_int(argc, argv, "--pid", -1);
    int budget = bbw_arg_int(argc, argv, "--budget-pct", 40);
    int win_ms = bbw_arg_int(argc, argv, "--window-ms", 1000);
    int per_ms = bbw_arg_int(argc, argv, "--period-ms", 50);
    int cpu    = bbw_arg_int(argc, argv, "--cpu", -1);
    if (pid <= 0) { fprintf(stderr, "[guard] need --pid\n"); return 1; }
    int n = win_ms / per_ms;
    if (n < 2) n = 2;
    if (n > MAXS) n = MAXS;

    clockid_t cid;
    int rc = clock_getcpuclockid((pid_t)pid, &cid);
    if (rc != 0) { fprintf(stderr, "[guard] cannot read pid %d's CPU clock: %s\n", pid, strerror(rc)); return 1; }
    bbw_rt_setup("guard", BBW_PRIO_GUARD, cpu);
    fprintf(stderr, "[guard] watching pid %d: budget %d%% of one CPU over %d ms\n", pid, budget, n * per_ms);

    int64_t used[MAXS], at[MAXS];
    int k = 0, filled = 0;
    double peak = 0;
    for (;;) {
        struct timespec ts;
        if (clock_gettime(cid, &ts) != 0 || kill(pid, 0) != 0) break;   /* candidate has gone */
        int64_t now = bbw_now_ns(), cpu_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        used[k] = cpu_ns; at[k] = now;
        int old = filled >= n ? (k + 1) % n : 0;
        if (filled >= 1) {
            double pct = 100.0 * (double)(cpu_ns - used[old]) / (double)(now - at[old] + 1);
            if (filled >= n - 1 && pct > peak) peak = pct;
            if (filled >= n - 1 && pct > budget) {
                kill(pid, SIGKILL);
                fprintf(stderr, "[guard] BUDGET EXCEEDED: pid %d used %.0f%% of a CPU over the last %d ms "
                                "(budget %d%%) -> candidate stopped (SIGKILL)\n", pid, pct, n * per_ms, budget);
                return 0;
            }
        }
        k = (k + 1) % n;
        if (filled < n) filled++;
        struct timespec sl = { per_ms / 1000, (long)(per_ms % 1000) * 1000000L };
        nanosleep(&sl, NULL);
    }
    fprintf(stderr, "[guard] pid %d ended within budget (peak %.1f%% of a CPU)\n", pid, peak);
    return 0;
}
