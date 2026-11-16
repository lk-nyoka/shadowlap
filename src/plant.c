/*
 * bbw_plant - the simulated car. It owns the clock.
 *
 * Every 10 ms:
 *   1. build a frame from the lap replay and the battery state
 *   2. hand it to v1 (production region) and drop a copy on the shadow ring
 *      (never waits for v2)
 *   3. at +8 ms, take whatever v1 has sent to the actuator for this frame;
 *      if nothing arrived it is a deadline miss and the discs do all the work
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

typedef struct { double t, dist, v, thr; int brake, zone; } lap_row_t;

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static lap_row_t *load_lap(const char *path, size_t *n_out) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); exit(1); }
    size_t cap = 16384, n = 0;
    lap_row_t *rows = malloc(cap * sizeof *rows);
    char line[256];
    if (!fgets(line, sizeof line, fp)) { fprintf(stderr, "empty lap file\n"); exit(1); }
    while (fgets(line, sizeof line, fp)) {
        lap_row_t r;
        double kmh;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%d,%d", &r.t, &r.dist, &kmh, &r.thr, &r.brake, &r.zone) != 6)
            continue;
        r.v = kmh / 3.6;
        if (n == cap) { cap *= 2; rows = realloc(rows, cap * sizeof *rows); }
        rows[n++] = r;
    }
    fclose(fp);
    if (n < 10) { fprintf(stderr, "lap file has too few rows\n"); exit(1); }
    double dt = (rows[n - 1].t - rows[0].t) / (double)(n - 1);
    if (fabs(dt - BBW_DT) > 0.002)
        fprintf(stderr, "[plant] warning: lap sample spacing %.4fs, expected %.4fs\n", dt, BBW_DT);
    *n_out = n;
    return rows;
}

int main(int argc, char **argv) {
    const char *lap_path = bbw_arg_str(argc, argv, "--lap", "data/lap_a.csv");
    double max_s   = bbw_arg_double(argc, argv, "--max-seconds", 0);
    double soc     = bbw_arg_double(argc, argv, "--soc0", 0.80);
    int    laps    = bbw_arg_int(argc, argv, "--laps", 1);
    int    cpu     = bbw_arg_int(argc, argv, "--cpu", -1);
    int    delay   = bbw_arg_int(argc, argv, "--start-delay-ms", 1500);

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    size_t nrows;
    lap_row_t *lap = load_lap(lap_path, &nrows);

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
    bbw_shadow_t *shad = bbw_shm_create(BBW_SHM_SHADOW, sizeof *shad, 0666);
    bbw_rec_t    *rec  = bbw_shm_create(BBW_SHM_REC,    sizeof *rec,  0600);
    sem_init(&prod->frame_ready, 1, 0);
    sem_init(&shad->frame_ready, 1, 0);
    atomic_store(&prod->plant_running, 1);
    atomic_store(&shad->plant_running, 1);
    atomic_store(&rec->plant_running, 1);
    prod->magic = shad->magic = rec->magic = BBW_MAGIC;

    /* Set priority BEFORE starting the actuator thread so it inherits it.
     * (Found by the flood test: an actuator thread at normal priority was
     * starved by shadow-side CPU burners, and v1's commands arrived late.) */
    bbw_rt_setup("plant", BBW_PRIO_PLANT, cpu);
    if (bbw_actuator_serve_start() != 0) { fprintf(stderr, "[plant] actuator failed\n"); return 1; }

    fprintf(stderr, "[plant] %zu samples (%.1fs lap), %d lap(s); starting in %d ms\n",
            nrows, lap[nrows - 1].t, laps, delay);
    usleep((useconds_t)delay * 1000);

    uint64_t total = (uint64_t)nrows * (uint64_t)laps;
    if (max_s > 0 && (uint64_t)(max_s * BBW_HZ) < total) total = (uint64_t)(max_s * BBW_HZ);

    double lap_harvest = 0;
    uint64_t misses = 0, clamps = 0, overruns = 0;
    int64_t t0 = bbw_now_ns() + BBW_PERIOD_NS;

    for (uint64_t k = 0; k < total && !g_stop; k++) {
        size_t i = (size_t)(k % nrows);
        if (i == 0) lap_harvest = 0;                       /* new lap */
        const lap_row_t *r = &lap[i];
        const lap_row_t *nx = &lap[i + 1 < nrows ? i + 1 : i];

        int64_t t_k = t0 + (int64_t)k * BBW_PERIOD_NS;
        bbw_sleep_until_ns(t_k);

        /* ---- 1. build the frame ---- */
        bbw_frame_t f;
        memset(&f, 0, sizeof f);
        f.seq = k;
        f.t_lap = r->t;
        f.dist_m = r->dist;
        f.v = r->v;
        f.throttle = r->thr;
        f.brake = r->brake;
        f.zone = r->zone;
        double a = (r->v - nx->v) / BBW_DT;
        f.a_req = (r->brake && a > 0) ? (a > 60 ? 60 : a) : 0;
        f.soc = soc;
        f.lap_harvest_j = lap_harvest;
        f.f_rear_req = CAR_REAR_BRAKE_SHARE * CAR_MASS_KG * f.a_req;
        f.f_rear_grip = car_rear_grip(f.v);
        f.t_ns = bbw_now_ns();

        /* ---- 2. publish: v1 first, then the shadow copy (never blocks) ---- */
        bbw_seqlock_write(prod, &f);
        sem_post(&prod->frame_ready);

        frame_ring_push(&shad->in, &f);
        int sv = 0;
        sem_getvalue(&shad->frame_ready, &sv);
        if (sv < 64) sem_post(&shad->frame_ready);        /* bounded, even if v2 is frozen */

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
            /* deadline miss: safe fallback, all braking on the discs */
            misses++;
            c.seq = k;
            c.f_regen = 0;
            c.f_friction = f.f_rear_req;
            c.t_done_ns = 0;
            rr.latency_ns = -1;
        }

        /* the plant enforces physics no matter what it is told */
        double p_lim = car_regen_power_limit(soc, lap_harvest);
        double p = c.f_regen * f.v;
        if (c.f_regen < 0 || p > p_lim * 1.001 + 1.0) {
            double reg = c.f_regen < 0 ? 0 : (f.v > 1 ? p_lim / f.v : 0);
            c.f_friction += c.f_regen - reg;
            c.f_regen = reg;
            p = reg * f.v;
            rr.clamped = 1;
            clamps++;
        }
        rr.cmd = c;
        rr.p_regen_w = p;

        /* ---- 4. battery: harvest under braking, deploy on full throttle ---- */
        soc += p * BBW_DT * BATT_CHARGE_EFF / BATT_USABLE_J;
        lap_harvest += p * BBW_DT;
        if (f.throttle > 0.9 && soc > DEPLOY_MIN_SOC)
            soc -= DEPLOY_W * f.throttle * BBW_DT / BATT_USABLE_J;
        if (soc > 1) soc = 1;
        if (soc < 0) soc = 0;

        rr.shadow_dropped = atomic_load(&shad->in.dropped);
        rr.plant_lag_ns = lag;
        rec_ring_push(&rec->cycles, &rr);
    }

    atomic_store(&prod->plant_running, 0);
    atomic_store(&shad->plant_running, 0);
    atomic_store(&rec->plant_running, 0);
    sem_post(&prod->frame_ready);
    sem_post(&shad->frame_ready);
    atomic_store(&rec->run_done, 1);

    fprintf(stderr, "[plant] done: %llu cycles, %llu v1 deadline misses, %llu plant overruns (host/OS late), "
                    "%llu clamps, %llu shadow frames dropped, %llu records dropped\n",
            (unsigned long long)total, (unsigned long long)misses, (unsigned long long)overruns,
            (unsigned long long)clamps,
            (unsigned long long)atomic_load(&shad->in.dropped),
            (unsigned long long)atomic_load(&rec->cycles.dropped));

    sleep(2);                       /* let the others drain and exit */
    bbw_actuator_serve_stop();
    bbw_shm_unlink_all();
    free(lap);
    return 0;
}
