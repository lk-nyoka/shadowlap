/*
 * cpuwatch - a CPU budget for one user's processes, enforced from outside.
 * Companion code for codelab/README.md.
 *
 *   cpuwatch --user NAME [--budget 40] [--window-ms 1000] [--period-ms 50] [--prio 64]
 *
 * Every period it reads the CPU-time clock of every process owned by NAME,
 * keeps a sliding window of the total, and when the user's processes used
 * more than --budget percent of one CPU over the window, it stops them all
 * with SIGKILL and says how long they ran hot.
 *
 * Builds on QNX 8.0:  qcc -Vgcc_ntox86_64 -O2 -Wall -o cpuwatch cpuwatch.c
 * and on Linux:       cc -O2 -Wall -o cpuwatch cpuwatch.c
 * Run it as root (it reads and stops another user's processes).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
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

#define MAXP 256          /* processes we track for the user */
#define MAXS 200          /* samples in the window */

/* ---- step 2: who owns a process ---------------------------------------- */
static long owner_uid(int pid) {
    char path[64];
#ifdef __QNX__
    /* QNX: ask the process manager through /proc/<pid>/as */
    snprintf(path, sizeof path, "/proc/%d/as", pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        procfs_info info;
        int rc = devctl(fd, DCMD_PROC_INFO, &info, sizeof info, NULL);
        close(fd);
        if (rc == EOK) return (long)info.euid;
    }
#endif
    /* Linux (and a fallback): /proc/<pid> is owned by the process's user */
    struct stat st;
    snprintf(path, sizeof path, "/proc/%d", pid);
    return stat(path, &st) == 0 ? (long)st.st_uid : -1;
}

/* ---- step 1: how much CPU a process has used, in nanoseconds ------------ */
static int64_t cpu_ns(int pid) {
    clockid_t cid;
    struct timespec ts;
    if (clock_getcpuclockid((pid_t)pid, &cid) != 0) return -1;
    if (clock_gettime(cid, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- step 3: list the user's processes and add up new CPU time ---------- */
static int g_pid[MAXP], g_n;
static int64_t g_seen[MAXP];

static int64_t sample(long uid, int *count) {
    int pid[MAXP], n = 0;
    int64_t seen[MAXP], delta = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && n < MAXP) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int p = atoi(e->d_name);
        if (p > 0 && owner_uid(p) == uid) pid[n++] = p;
    }
    if (d) closedir(d);
    for (int i = 0; i < n; i++) {
        int64_t before = 0, now = cpu_ns(pid[i]);
        for (int j = 0; j < g_n; j++)
            if (g_pid[j] == pid[i]) { before = g_seen[j]; break; }
        if (now < 0) now = before;                /* it exited meanwhile */
        if (now > before) delta += now - before;  /* a new process counts from birth */
        seen[i] = now;
    }
    memcpy(g_pid, pid, sizeof(int) * (size_t)n);
    memcpy(g_seen, seen, sizeof(int64_t) * (size_t)n);
    g_n = n;
    *count = n;
    return delta;
}

static void stop_all(long uid) {
    for (int round = 0; round < 5; round++) {     /* again, in case one forked */
        for (int i = 0; i < g_n; i++) kill(g_pid[i], SIGKILL);
        struct timespec sl = { 0, 5000000L };
        nanosleep(&sl, NULL);
        int n;
        sample(uid, &n);
        if (n == 0) return;
    }
}

static const char *arg(int argc, char **argv, const char *name, const char *def) {
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

int main(int argc, char **argv) {
    const char *user = arg(argc, argv, "--user", NULL);
    int budget = atoi(arg(argc, argv, "--budget", "40"));
    int win_ms = atoi(arg(argc, argv, "--window-ms", "1000"));
    int per_ms = atoi(arg(argc, argv, "--period-ms", "50"));
    int prio   = atoi(arg(argc, argv, "--prio", "64"));
    struct passwd *pw = user ? getpwnam(user) : NULL;
    if (!pw) { fprintf(stderr, "usage: cpuwatch --user NAME [--budget 40] ...\n"); return 1; }
    if (pw->pw_uid == 0 || pw->pw_uid == getuid()) {
        fprintf(stderr, "cpuwatch: refusing to watch root or my own user\n");
        return 1;
    }
    long uid = (long)pw->pw_uid;

    /* ---- step 4: run above anything the watched user can reach ---------- */
    struct sched_param sp = { .sched_priority = prio };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "cpuwatch: warning: could not get SCHED_FIFO %d (not root?)\n", prio);

    int n = win_ms / per_ms;
    if (n < 2) n = 2;
    if (n > MAXS) n = MAXS;
    int64_t used[MAXS], at[MAXS], total = 0, onset = 0, last_total = -1, last_at = 0;
    int k = 0, filled = 0;
    printf("cpuwatch: user %s: budget %d%% of one CPU over %d ms, checked every %d ms\n",
           user, budget, n * per_ms, per_ms);
    fflush(stdout);

    /* ---- step 3 (cont.): the sliding window ----------------------------- */
    for (;;) {
        int count;
        total += sample(uid, &count);
        int64_t now = mono_ns();
        if (last_total >= 0) {          /* remember when it started running hot */
            double inst = 100.0 * (double)(total - last_total) / (double)(now - last_at + 1);
            if (inst > budget) { if (!onset) onset = last_at; } else onset = 0;
        }
        last_total = total; last_at = now;
        used[k] = total; at[k] = now;
        int old = filled >= n ? (k + 1) % n : 0;
        /* act only on a full window, so a short burst is allowed */
        if (filled >= n - 1) {
            double pct = 100.0 * (double)(total - used[old]) / (double)(now - at[old] + 1);
            if (pct > budget) {
                stop_all(uid);
                printf("cpuwatch: OVER BUDGET: %d process(es) of %s used %.0f%% of a CPU "
                       "-> stopped (SIGKILL) after running hot for %.0f ms\n",
                       count, user, pct, onset ? (double)(mono_ns() - onset) / 1e6 : -1.0);
                fflush(stdout);
                onset = 0; filled = 0; k = 0;       /* keep watching the user */
                continue;
            }
        }
        k = (k + 1) % n;
        if (filled < n) filled++;
        struct timespec sl = { per_ms / 1000, (long)(per_ms % 1000) * 1000000L };
        nanosleep(&sl, NULL);
    }
}
