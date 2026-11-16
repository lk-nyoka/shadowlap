/*
 * bbw_v1 - the production brake controller. It is in charge.
 *
 * It only touches the production region and the actuator. It has no idea
 * the shadow exists, so nothing the shadow does can make it wait.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "bbw.h"
#include "control.h"

int main(int argc, char **argv) {
    int cpu = bbw_arg_int(argc, argv, "--cpu", -1);

    bbw_prod_t *prod = bbw_shm_open(BBW_SHM_PROD, sizeof *prod, 10000);
    if (!prod) { fprintf(stderr, "[v1] cannot open %s: %s\n", BBW_SHM_PROD, strerror(errno)); return 1; }
    int act = bbw_actuator_open();
    if (act < 0) { fprintf(stderr, "[v1] cannot open actuator: %s\n", strerror(errno)); return 1; }
    bbw_rt_setup("v1", BBW_PRIO_V1, cpu);
    fprintf(stderr, "[v1] running\n");

    uint64_t last = UINT64_MAX, handled = 0;
    while (1) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        if (sem_timedwait(&prod->frame_ready, &ts) != 0) {
            if (errno == EINTR) continue;
            if (!atomic_load(&prod->plant_running) && handled) break;
            continue;
        }
        if (!atomic_load(&prod->plant_running)) break;

        bbw_frame_t f;
        bbw_seqlock_read(prod, &f);
        if (f.seq == last) continue;           /* already answered */
        last = f.seq;

        bbw_cmd_t c;
        strategy_v1(&f, &c);
        c.t_done_ns = bbw_now_ns();
        if (bbw_actuator_send(act, &c) != 0) {
            fprintf(stderr, "[v1] actuator write failed: %s\n", strerror(errno));
            break;
        }
        handled++;
    }
    fprintf(stderr, "[v1] done: %llu frames handled\n", (unsigned long long)handled);
    close(act);
    return 0;
}
