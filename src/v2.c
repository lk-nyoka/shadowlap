/*
 * bbw_v2 - the shadow brake controller. It watches; it cannot brake.
 *
 * It opens only the shadow region. Its decisions go to the comparer, never
 * to the actuator. Built-in faults make the demo repeatable:
 *   --crash-at SEQ          crash (SIGSEGV) when it reaches frame SEQ
 *   --freeze-at SEQ --freeze-ms MS   hang for MS milliseconds at frame SEQ
 *   --slow-ms MS            take MS extra milliseconds on every frame
 *   --attack                try to open and write the actuator, report result
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "bbw.h"
#include "control.h"

static int try_attack(void) {
    int fd = bbw_actuator_open();
    if (fd < 0) {
        fprintf(stderr, "[v2] ATTACK BLOCKED: opening %s failed: %s\n",
                BBW_ACTUATOR_PATH, strerror(errno));
        return 0;
    }
    bbw_cmd_t evil = { .seq = 0, .f_regen = 1e6, .f_friction = 0 };
    int ok = bbw_actuator_send(fd, &evil) == 0;
    fprintf(stderr, ok ? "[v2] ATTACK NOT BLOCKED: wrote to %s (no isolation enforced on this system)\n"
                       : "[v2] ATTACK BLOCKED: write to %s failed\n", BBW_ACTUATOR_PATH);
    close(fd);
    return ok;
}

int main(int argc, char **argv) {
    long long crash_at  = bbw_arg_int(argc, argv, "--crash-at", -1);
    long long freeze_at = bbw_arg_int(argc, argv, "--freeze-at", -1);
    int freeze_ms = bbw_arg_int(argc, argv, "--freeze-ms", 2000);
    int slow_ms   = bbw_arg_int(argc, argv, "--slow-ms", 0);
    int cpu       = bbw_arg_int(argc, argv, "--cpu", -1);

    if (bbw_arg_flag(argc, argv, "--attack")) {
        /* the attack needs the plant's actuator to exist; give it a moment */
        return try_attack() ? 2 : 0;
    }

    /* The production region must be unreachable for v2. Show it. */
    if (bbw_shm_open(BBW_SHM_PROD, sizeof(bbw_prod_t), 0) != NULL)
        fprintf(stderr, "[v2] note: production region is readable by v2 on this system "
                        "(on QNX, run v2 as a separate user to deny this)\n");

    bbw_shadow_t *sh = bbw_shm_open(BBW_SHM_SHADOW, sizeof *sh, 10000);
    if (!sh) { fprintf(stderr, "[v2] cannot open %s: %s\n", BBW_SHM_SHADOW, strerror(errno)); return 1; }
    bbw_rt_setup("v2", BBW_PRIO_V2, cpu);
    frame_ring_skip_to_head(&sh->in);          /* ignore any stale backlog */
    fprintf(stderr, "[v2] shadowing (pid %d)\n", (int)getpid());

    uint64_t handled = 0;
    while (1) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        sem_timedwait(&sh->frame_ready, &ts);

        bbw_frame_t f;
        int got = 0;
        while (frame_ring_pop(&sh->in, &f)) {
            got = 1;
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
            strategy_v2(&f, &c);
            c.t_done_ns = bbw_now_ns();
            cmd_ring_push(&sh->out, &c);
            atomic_store(&sh->v2_heartbeat_ns, c.t_done_ns);
            handled++;
        }
        if (!got && !atomic_load(&sh->plant_running)) break;
    }
    fprintf(stderr, "[v2] done: %llu frames shadowed\n", (unsigned long long)handled);
    return 0;
}
