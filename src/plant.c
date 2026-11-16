/*
 * bbw_plant - the simulated car. It owns the clock.
 *
 * Every 10 ms:
 *   1. build a frame from the lap replay and the battery state
 *   2. hand it to v1 (production region) and publish a copy that v2 can only
 *      read (the plant never waits for, or hears from, v2)
 *   3. at +8 ms, take whatever v1 has sent to the actuator for this frame;
 *      if nothing arrived it is a deadline miss and the discs take over
 *      (limited to the rear grip)
 *   4. update the battery, write a record for the comparer
 *
 * Speed comes straight from the lap trace (kinematic replay). The controllers
 * only decide the rear split between motor harvesting and brake discs.
 */
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bbw.h"
#include "control.h"
#include "vehicle.h"

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    const char *lap_path = bbw_arg_str(argc, argv, "--lap", "data/lap_a.csv");
    double max_s   = bbw_arg_double(argc, argv, "--max-seconds", 0);
    double soc0    = bbw_arg_double(argc, argv, "--soc0", 0.80);
    double ambient = bbw_arg_double(argc, argv, "--ambient", 30.0);
    double mu      = bbw_arg_double(argc, argv, "--grip", 1.0);
    int    laps    = bbw_arg_int(argc, argv, "--laps", 1);
    int    cpu     = bbw_arg_int(argc, argv, "--cpu", -1);
    int    delay   = bbw_arg_int(argc, argv, "--start-delay-ms", 1500);

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    size_t nrows;
    lap_row_t *lap = vehicle_load_lap(lap_path, &nrows);

    /* used by run_demo.sh so the target needs no awk: print the frame where
     * the first braking zone starts, then exit */
    if (bbw_arg_flag(argc, argv, "--first-brake")) {
        size_t i = 0;
        while (i < nrows && !lap[i].brake) i++;
        printf("%zu\n", i < nrows ? i : 0);
        free(lap);
        return 0;
    }

    bbw_prod_t   *prod = bbw_shm_create(BBW_SHM_PROD,   sizeof *prod, 0600);
    bbw_shin_t   *shin = bbw_shm_create(BBW_SHM_SHIN,   sizeof *shin, 0644);  /* v2: read only */
    bbw_shout_t  *shout= bbw_shm_create(BBW_SHM_SHOUT,  sizeof *shout, 0666); /* v2 writes here */
    bbw_rec_t    *rec  = bbw_shm_create(BBW_SHM_REC,    sizeof *rec,  0600);
    sem_init(&prod->frame_ready, 1, 0);
    atomic_store(&prod->plant_running, 1);
    atomic_store(&shin->plant_running, 1);
    atomic_store(&rec->plant_running, 1);
    prod->magic = shin->magic = shout->magic = rec->magic = BBW_MAGIC;

    /* Set priority BEFORE starting the actuator thread so it inherits it.
     * (Found by the flood test: an actuator thread at normal priority was
     * starved by shadow-side CPU burners, and v1's commands arrived late.) */
    int plant_rt = bbw_rt_setup("plant", BBW_PRIO_PLANT, cpu);
    if (bbw_actuator_serve_start() != 0) { fprintf(stderr, "[plant] actuator failed\n"); return 1; }

    fprintf(stderr, "[plant] %zu samples (%.1fs lap), %d lap(s); starting in %d ms\n",
            nrows, lap[nrows - 1].t, laps, delay);
    usleep((useconds_t)delay * 1000);

    uint64_t total = (uint64_t)nrows * (uint64_t)laps;
    if (max_s > 0 && (uint64_t)(max_s * BBW_HZ) < total) total = (uint64_t)(max_s * BBW_HZ);

    vehicle_t car;
    vehicle_init(&car, soc0, ambient, mu);
    uint64_t misses = 0, clamps = 0, overruns = 0, fallback_limited = 0;
    int64_t t0 = bbw_now_ns() + BBW_PERIOD_NS;

    for (uint64_t k = 0; k < total && !g_stop; k++) {
        size_t i = (size_t)(k % nrows);
        if (i == 0) vehicle_new_lap(&car);
        const lap_row_t *r = &lap[i];
        const lap_row_t *nx = &lap[i + 1 < nrows ? i + 1 : i];

        int64_t t_k = t0 + (int64_t)k * BBW_PERIOD_NS;
        bbw_sleep_until_ns(t_k);

        /* ---- 1. build the frame ---- */
        bbw_frame_t f;
        vehicle_frame(&car, r, nx, k, &f);
        f.t_ns = bbw_now_ns();

        /* ---- 2. publish: v1 first, then the shadow copy (never blocks) ---- */
        bbw_seqlock_write(prod, &f);
        sem_post(&prod->frame_ready);

        bcast_publish(&shin->in, &f);      /* overwrite-oldest: v2 can never hold the plant up */

        /* ---- 3. collect v1's answer at the deadline ----
         * v1's 8 ms starts when it is actually given the frame. If the plant
         * itself woke up late (host or OS stall), that is counted separately
         * as a plant overrun, so v1 is not blamed for time it never had. */
        int64_t lag = f.t_ns - t_k;
        if (lag > BBW_PLANT_LATE_NS) overruns++;
        bbw_sleep_until_ns(f.t_ns + BBW_DEADLINE_NS);
        bbw_record_t rr;
        memset(&rr, 0, sizeof rr);
        rr.f = f;
        bbw_cmd_t c;
        int64_t recv_ns = 0;
        if (bbw_actuator_latest(&c, &recv_ns) && c.seq == k) {
            rr.applied = 1;
            rr.latency_ns = recv_ns - f.t_ns;
        } else {
            /* deadline miss: fallback = no motor braking, discs only, and never
             * more than 95% of the rear grip (the request alone could exceed it;
             * the rest is the front axle's job). Illustrative, not a real car's
             * degraded mode: see docs/how-it-works.md. */
            misses++;
            c.seq = k;
            c.f_regen = 0;
            c.f_friction = f.f_rear_req;
            if (c.f_friction > 0.95 * f.f_rear_grip) { c.f_friction = 0.95 * f.f_rear_grip; fallback_limited++; }
            c.t_done_ns = 0;
            rr.latency_ns = -1;
        }

        /* ---- 4. the car enforces physics, then moves on 10 ms ---- */
        double p = 0;
        if (vehicle_apply(&car, &f, &c, &p)) { rr.clamped = 1; clamps++; }
        rr.cmd = c;
        rr.p_regen_w = p;

        rr.shadow_dropped = 0;             /* the comparer counts what v2 missed */
        rr.plant_lag_ns = lag;
        rec_ring_push(&rec->cycles, &rr);
    }

    rec->plant_rt_ok = plant_rt ? 1 : -1;
    rec->v1_rt_ok = atomic_load(&prod->v1_rt_ok);
    rec->actuator_rejects = bbw_actuator_rejects();
    rec->fallbacks = misses;
    atomic_store(&prod->plant_running, 0);
    atomic_store(&shin->plant_running, 0);
    atomic_store(&rec->plant_running, 0);
    sem_post(&prod->frame_ready);
    atomic_store(&rec->run_done, 1);

    fprintf(stderr, "[plant] done: %llu cycles, %llu v1 deadline misses, %llu plant overruns (host/OS late), "
                    "%llu clamps, %llu fallbacks limited by grip, %llu actuator commands rejected, %llu records dropped\n",
            (unsigned long long)total, (unsigned long long)misses, (unsigned long long)overruns,
            (unsigned long long)clamps, (unsigned long long)fallback_limited,
            (unsigned long long)rec->actuator_rejects,
            (unsigned long long)atomic_load(&rec->cycles.dropped));

    sleep(2);                       /* let the others drain and exit */
    bbw_actuator_serve_stop();
    bbw_shm_unlink_all();
    free(lap);
    return 0;
}
