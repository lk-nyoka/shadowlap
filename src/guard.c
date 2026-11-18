/*
 * bbw_guard - an outside CPU budget for the shadow candidate.
 *
 * QNX 8.0 has no adaptive partitioning (discontinued in SDP 8.0); QNX points
 * to sporadic scheduling and per-thread/process runtime clocks instead.
 * Sporadic scheduling only limits a thread that chooses it, so a hostile
 * candidate could switch it off. This guard uses the other tool: it reads the
 * candidate's CPU-time clocks from OUTSIDE, every 50 ms, and if the candidate
 * used more than its budget over the last second it is stopped (SIGKILL) and
 * the event is logged. The guard runs at a privileged priority (QNX: 64, just
 * above anything an unprivileged process can reach, below v1 and the car), so
 * the candidate cannot starve it.
 *
 * With --user, the budget covers EVERY process owned by the candidate's user,
 * not one process ID: a candidate that starts a child process to do its work
 * cannot hide that work from the guard, and anything the user still has
 * running when the candidate ends is stopped too.
 *
 *   bbw_guard --pid P [--user NAME] [--budget-pct 40] [--window-ms 1000] [--period-ms 50] [--cpu C]
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __QNX__
#include <devctl.h>
#include <sys/procfs.h>
#endif
#include "bbw.h"

#define MAXS 256      /* samples in the window */
#define MAXP 256      /* processes of the candidate user we track */

/* effective user of a process, or -1 if it is gone / unreadable */
static long owner_uid(int pid) {
    char path[64];
#ifdef __QNX__
    snprintf(path, sizeof path, "/proc/%d/as", pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        procfs_info info;
        int rc = devctl(fd, DCMD_PROC_INFO, &info, sizeof info, NULL);
        close(fd);
        if (rc == EOK) return (long)info.euid;
    }
#endif
    struct stat st;
    snprintf(path, sizeof path, "/proc/%d", pid);
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_uid;           /* Linux: /proc/<pid> is owned by the process's euid */
}

static int64_t cpu_ns_of(int pid) {
    clockid_t cid;
    struct timespec ts;
    if (clock_getcpuclockid((pid_t)pid, &cid) != 0 || clock_gettime(cid, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* the processes being watched, with the CPU time already counted for each */
static int g_pid[MAXP], g_np;
static int64_t g_seen[MAXP];

/* Refresh the list (all processes of uid, plus the main pid) and return the CPU
 * used since the last call. *count gets the number of live processes;
 * returns -1 if there are more than MAXP of them (a fork flood). */
static int64_t sample(int main_pid, long uid, int *count) {
    int pid[MAXP], np = 0, flood = 0;
    int64_t seen[MAXP];
    if (uid >= 0) {
        DIR *d = opendir("/proc");
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            if (!isdigit((unsigned char)e->d_name[0])) continue;
            int p = atoi(e->d_name);
            if (p <= 0 || p == main_pid || owner_uid(p) != uid) continue;
            if (np >= MAXP - 1) { flood = 1; break; }
            pid[np++] = p;
        }
        if (d) closedir(d);
    }
    if (kill(main_pid, 0) == 0) pid[np++] = main_pid;
    int64_t delta = 0;
    for (int i = 0; i < np; i++) {
        int64_t now = cpu_ns_of(pid[i]), before = 0;
        for (int j = 0; j < g_np; j++) if (g_pid[j] == pid[i]) { before = g_seen[j]; break; }
        if (now < 0) now = before;                 /* exited between listing and reading */
        if (now > before) delta += now - before;   /* a new process counts from its start */
        seen[i] = now;
    }
    memcpy(g_pid, pid, sizeof(int) * (size_t)np);
    memcpy(g_seen, seen, sizeof(int64_t) * (size_t)np);
    g_np = np;
    *count = np;
    return flood ? -1 : delta;
}

static void stop_all(int main_pid, long uid) {
    for (int round = 0; round < 5; round++) {          /* again, in case one forked meanwhile */
        for (int i = 0; i < g_np; i++) kill(g_pid[i], SIGKILL);
        struct timespec sl = { 0, 5000000L };
        nanosleep(&sl, NULL);
        int n = 0;
        (void)sample(main_pid, uid, &n);
        if (n == 0) return;
    }
}

int main(int argc, char **argv) {
    int pid    = bbw_arg_int(argc, argv, "--pid", -1);
    const char *user = bbw_arg_str(argc, argv, "--user", NULL);
    int budget = bbw_arg_int(argc, argv, "--budget-pct", 40);
    int win_ms = bbw_arg_int(argc, argv, "--window-ms", 1000);
    int per_ms = bbw_arg_int(argc, argv, "--period-ms", 50);
    int cpu    = bbw_arg_int(argc, argv, "--cpu", -1);
    if (pid <= 0) { fprintf(stderr, "[guard] need --pid\n"); return 1; }
    long uid = -1;
    if (user) {
        struct passwd *pw = getpwnam(user);
        if (!pw) { fprintf(stderr, "[guard] no such user %s\n", user); return 1; }
        if (pw->pw_uid == 0 || pw->pw_uid == getuid()) {
            fprintf(stderr, "[guard] refusing to watch user %s: it is root or the guard's own user\n", user);
            return 1;
        }
        uid = (long)pw->pw_uid;
    }
    int n = win_ms / per_ms;
    if (n < 2) n = 2;
    if (n > MAXS) n = MAXS;
    if (cpu_ns_of(pid) < 0) { fprintf(stderr, "[guard] cannot read pid %d's CPU clock: %s\n", pid, strerror(errno)); return 1; }
    bbw_rt_setup("guard", BBW_PRIO_GUARD, cpu);
    if (user)
        fprintf(stderr, "[guard] watching pid %d and every process of user %s: budget %d%% of one CPU over %d ms\n",
                pid, user, budget, n * per_ms);
    else
        fprintf(stderr, "[guard] watching pid %d: budget %d%% of one CPU over %d ms\n", pid, budget, n * per_ms);

    int64_t used[MAXS], at[MAXS], total = 0;
    int k = 0, filled = 0, most = 0;
    double peak = 0;
    int64_t onset = 0, last_total = -1, last_at = 0;   /* when the candidate started running hot */
    for (;;) {
        int count = 0;
        int64_t d = sample(pid, uid, &count);
        int64_t now = bbw_now_ns();
        if (d < 0) {
            stop_all(pid, uid);
            fprintf(stderr, "[guard] BUDGET EXCEEDED: user %s has more than %d processes (fork flood) "
                            "-> all stopped (SIGKILL)\n", user, MAXP - 1);
            return 0;
        }
        if (count > most) most = count;
        if (count == 0) break;                     /* the candidate and all its processes have gone */
        if (kill(pid, 0) != 0) {                   /* main candidate gone, leftovers remain */
            stop_all(pid, uid);
            fprintf(stderr, "[guard] candidate ended; stopped %d leftover process(es) of user %s\n", count, user);
            break;
        }
        total += d;
        /* usage over the last period alone: marks the moment it started burning */
        if (last_total >= 0) {
            double inst = 100.0 * (double)(total - last_total) / (double)(now - last_at + 1);
            if (inst > budget) { if (!onset) onset = last_at; } else onset = 0;
        }
        last_total = total; last_at = now;
        used[k] = total; at[k] = now;
        int old = filled >= n ? (k + 1) % n : 0;
        if (filled >= 1) {
            double pct = 100.0 * (double)(total - used[old]) / (double)(now - at[old] + 1);
            if (filled >= n - 1 && pct > peak) peak = pct;
            if (filled >= n - 1 && pct > budget) {
                stop_all(pid, uid);
                int64_t stopped = bbw_now_ns();
                fprintf(stderr, "[guard] BUDGET EXCEEDED: %d process(es) of the candidate used %.0f%% of a CPU "
                                "over the last %d ms (budget %d%%) -> candidate stopped (SIGKILL)\n",
                        count, pct, n * per_ms, budget);
                fprintf(stderr, "[guard] containment: candidate ran hot for %.0f ms before it was stopped "
                                "(design limit: about %d ms)\n",
                        onset ? (double)(stopped - onset) / 1e6 : -1.0, n * per_ms + per_ms);
                return 0;
            }
        }
        k = (k + 1) % n;
        if (filled < n) filled++;
        struct timespec sl = { per_ms / 1000, (long)(per_ms % 1000) * 1000000L };
        nanosleep(&sl, NULL);
    }
    fprintf(stderr, "[guard] pid %d ended within budget (peak %.1f%% of a CPU, at most %d process(es))\n",
            pid, peak, most);
    return 0;
}
