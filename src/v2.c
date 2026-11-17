/*
 * bbw_v2 - the shadow brake controller. It watches; it cannot brake.
 *
 * It opens only the shadow region. Its decisions go to the comparer, never
 * to the actuator. Built-in faults make the demo repeatable:
 *   --crash-at SEQ          crash (SIGSEGV) when it reaches frame SEQ
 *   --freeze-at SEQ --freeze-ms MS   hang for MS milliseconds at frame SEQ
 *   --slow-ms MS            take MS extra milliseconds on every frame
 *   --attack [--v1-pid P --v1-cpu C --hog-seconds S]   try every route to the trusted side,
 *                           report each, then burn v1's CPU at the highest priority v2 can get
 *   --candidate v2|v2.1     which candidate strategy to run (default v2)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "bbw.h"
#include "control.h"
#ifdef __QNX__
#include <sys/neutrino.h>
#endif

/* --attack: a hostile v2 tries every route to the trusted side it can think
 * of. Each line says BLOCKED or NOT BLOCKED; exit code = number not blocked.
 * Nothing is actually harmed: the signal probe uses signal 0 (permission
 * check only) and any priority or CPU change is undone at once. */
static int g_open_routes;
static void probe(const char *what, int blocked, const char *why) {
    fprintf(stderr, "[v2] ATTACK %s: %s%s%s\n", blocked ? "BLOCKED" : "NOT BLOCKED", what,
            why ? " -> " : "", why ? why : "");
    if (!blocked) g_open_routes++;
}

static int try_attack(int v1_pid, int v1_cpu, int hog_s) {
    /* 1. command the brakes */
    int fd = bbw_actuator_open();
    if (fd < 0) probe("open the brake actuator " BBW_ACTUATOR_PATH, 1, strerror(errno));
    else {
        bbw_cmd_t evil = { .seq = 0, .f_regen = 1e6, .f_friction = 0 };
        int ok = bbw_actuator_send(fd, &evil) == 0;
        probe("write to the brake actuator", !ok, ok ? "write accepted (the actuator still rejects absurd values)" : strerror(errno));
        close(fd);
    }
    /* 2. read or change what v1 sees */
    probe("open v1's region " BBW_SHM_PROD, bbw_shm_open(BBW_SHM_PROD, sizeof(bbw_prod_t), 0) == NULL, strerror(errno));
    /* 3. tamper with the evidence */
    probe("open the evidence region " BBW_SHM_REC, bbw_shm_open(BBW_SHM_REC, sizeof(bbw_rec_t), 0) == NULL, strerror(errno));
    /* 4. change its own inputs (it may only read them) */
    probe("open its input region " BBW_SHM_SHIN " for writing",
          bbw_shm_open(BBW_SHM_SHIN, sizeof(bbw_shin_t), 0) == NULL, strerror(errno));
    /* 5. signal (stop/kill) v1 */
    if (v1_pid > 0) {
        int r = kill(v1_pid, 0);
        probe("signal v1 (stop or kill it)", r != 0, r != 0 ? strerror(errno) : "permitted");
    }
    /* 6. out-rank v1: raise its own priority above v1's */
    struct sched_param sp = { .sched_priority = BBW_PRIO_V1 + 1 }, old;
    int pol = 0;
    pthread_getschedparam(pthread_self(), &pol, &old);
    int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    probe("raise its priority above v1's", r != 0, r != 0 ? strerror(r) :
          "permitted: v1 must sit in a privileged priority band");
    if (r == 0) pthread_setschedparam(pthread_self(), pol, &old);
    /* the highest real-time priority it CAN reach (information, not a route) */
    int top = 0;
    for (int pr = BBW_PRIO_V1 - 1; pr >= 1 && !top; pr--) {
        struct sched_param t = { .sched_priority = pr };
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &t) == 0) top = pr;
    }
    pthread_setschedparam(pthread_self(), pol, &old);
    if (top) fprintf(stderr, "[v2] note: highest priority v2 can reach is %d; v1 runs at %d, the car at %d\n",
                     top, BBW_PRIO_V1, BBW_PRIO_PLANT);
    else     fprintf(stderr, "[v2] note: v2 cannot get any real-time priority\n");
    /* 7. move onto v1's CPU: allowed is not a breach while v1 out-ranks it */
    int moved = 0;
    if (v1_cpu >= 0) {
#ifdef __QNX__
        moved = ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << v1_cpu)) != -1;
#elif defined(__linux__)
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(v1_cpu, &set);
        moved = sched_setaffinity(0, sizeof set, &set) == 0;
#endif
        if (!moved) probe("move onto v1's CPU", 1, strerror(errno));
        else fprintf(stderr, "[v2] ATTACK CONTAINED: move onto v1's CPU -> allowed, but v1 (%d) and the car (%d) "
                             "out-rank anything v2 can reach (%d)\n", BBW_PRIO_V1, BBW_PRIO_PLANT, top);
    }
    /* 8. hide CPU use from the guard in a child process it is not watching */
    pid_t child = fork();
    if (child == 0) { volatile uint64_t x = 0; for (;;) x++; }
    if (child > 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); }
    probe("start a child process (to hide CPU use from the guard)", child < 0, child < 0 ? strerror(errno) : "permitted");
    /* 9. exhaust memory */
    size_t big = (size_t)1 << 30;
    char *m = malloc(big);
    if (m) { for (size_t i = 0; i < big; i += 4096) m[i] = 1; free(m); }
    probe("allocate and touch 1 GB of memory", m == NULL, m == NULL ? "refused (memory limit)" : "permitted");
    /* 10. exhaust file descriptors */
    int opened = 0, fds[4096];
    for (int i = 0; i < 4096; i++) { fds[i] = open("/dev/null", O_RDONLY); if (fds[i] < 0) break; opened++; }
    for (int i = 0; i < opened; i++) close(fds[i]);
    probe("open 4096 files", opened < 4096, opened < 4096 ? "refused (open-file limit)" : "permitted");

    /* 11. the real test: burn v1's CPU at the highest priority v2 can get */
    if (hog_s > 0) {
        if (top) { struct sched_param t = { .sched_priority = top }; pthread_setschedparam(pthread_self(), SCHED_FIFO, &t); }
        fprintf(stderr, "[v2] attack: spinning on %s at priority %d for %d s while the lap runs\n",
                moved ? "v1's CPU" : "its own CPU", top, hog_s);
        int64_t end = bbw_now_ns() + (int64_t)hog_s * 1000000000LL;
        volatile uint64_t x = 0;
        while (bbw_now_ns() < end) x++;
        pthread_setschedparam(pthread_self(), pol, &old);
        fprintf(stderr, "[v2] attack: stopped spinning (check v1 deadline misses in the plant line)\n");
    }
    fprintf(stderr, "[v2] attack summary: %d route(s) not blocked\n", g_open_routes);
    return g_open_routes;
}

int main(int argc, char **argv) {
    long long crash_at  = bbw_arg_int(argc, argv, "--crash-at", -1);
    long long freeze_at = bbw_arg_int(argc, argv, "--freeze-at", -1);
    int freeze_ms = bbw_arg_int(argc, argv, "--freeze-ms", 2000);
    int slow_ms   = bbw_arg_int(argc, argv, "--slow-ms", 0);
    int cpu       = bbw_arg_int(argc, argv, "--cpu", -1);
    const char *cand = bbw_arg_str(argc, argv, "--candidate", "v2");

    if (!candidate_known(cand)) { fprintf(stderr, "[v2] unknown candidate %s\n", cand); return 1; }
    if (bbw_arg_flag(argc, argv, "--attack"))
        return try_attack(bbw_arg_int(argc, argv, "--v1-pid", -1), bbw_arg_int(argc, argv, "--v1-cpu", -1),
                          bbw_arg_int(argc, argv, "--hog-seconds", 0)) ? 2 : 0;

    /* The production region must be unreachable for v2. Show it. */
    if (bbw_shm_open(BBW_SHM_PROD, sizeof(bbw_prod_t), 0) != NULL)
        fprintf(stderr, "[v2] note: production region is readable by v2 on this system "
                        "(run v2 as a separate user to deny this)\n");

    const bbw_shin_t *in = bbw_shm_open_ro(BBW_SHM_SHIN, sizeof *in, 10000);
    if (!in) { fprintf(stderr, "[v2] cannot open %s: %s\n", BBW_SHM_SHIN, strerror(errno)); return 1; }
    bbw_shout_t *out = bbw_shm_open(BBW_SHM_SHOUT, sizeof *out, 10000);
    if (!out) { fprintf(stderr, "[v2] cannot open %s: %s\n", BBW_SHM_SHOUT, strerror(errno)); return 1; }
    bbw_rt_setup("v2", BBW_PRIO_V2, cpu);
    /* start from the newest frame: a restarted v2 does not replay a backlog */
    uint64_t next = atomic_load_explicit(&((bcast_ring_t *)&in->in)->head, memory_order_acquire);
    fprintf(stderr, "[v2] shadowing as candidate %s (pid %d), inputs mapped read-only\n", cand, (int)getpid());

    uint64_t handled = 0, lost = 0;
    int idle_ms = 0;
    while (1) {
        bbw_frame_t f;
        int r = bcast_read(&in->in, next, &f);
        if (r == 0) {                                   /* nothing new: poll again in 1 ms */
            if (!atomic_load(&((bbw_shin_t *)in)->plant_running) && ++idle_ms > 200) break;
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
            continue;
        }
        idle_ms = 0;
        if (r < 0) {                                    /* fell behind: skip to what still exists */
            uint64_t h = atomic_load_explicit(&((bcast_ring_t *)&in->in)->head, memory_order_acquire);
            uint64_t oldest = h > BBW_RING_N / 2 ? h - BBW_RING_N / 2 : 0;
            if (next < oldest) { lost += oldest - next; next = oldest; } else { lost++; next++; }
            atomic_store(&out->v2_lost, lost);
            continue;
        }
        next++;
        if (crash_at >= 0 && (long long)f.seq >= crash_at) {
            fprintf(stderr, "[v2] injected crash at frame %llu\n", (unsigned long long)f.seq);
            raise(SIGSEGV);
        }
        if (freeze_at >= 0 && (long long)f.seq >= freeze_at) {
            fprintf(stderr, "[v2] injected freeze for %d ms at frame %llu\n",
                    freeze_ms, (unsigned long long)f.seq);
            usleep((useconds_t)freeze_ms * 1000);
            freeze_at = -1;
        }
        if (slow_ms > 0) usleep((useconds_t)slow_ms * 1000);

        bbw_cmd_t c;
        strategy_candidate(cand, &f, &c);
        c.t_done_ns = bbw_now_ns();
        cmd_ring_push(&out->out, &c);
        atomic_store(&out->v2_heartbeat_ns, c.t_done_ns);
        handled++;
    }
    fprintf(stderr, "[v2] done: %llu frames shadowed, %llu lost\n", (unsigned long long)handled,
            (unsigned long long)lost);
    return 0;
}
