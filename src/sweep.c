/*
 * bbw_sweep - many laps, many conditions, in seconds.
 *
 * The live demo proves timing and isolation on QNX. This tool answers the
 * other question an engineer asks: across lots of laps and conditions, how
 * often would v2 be unsafe, and what would it gain? It uses the same car
 * model (vehicle.c) and the same two strategies (control.c), with no
 * real-time clock, so it runs anywhere, including in CI.
 *
 *   bbw_sweep --lap data/lap_a.csv --runs 200 --laps 3 --seed 1 --out out/sweep [--candidate v2.1]
 *
 * Each run draws: starting battery 50-92%, air temperature 10-45 C,
 * track grip 85-105% of nominal. A problem counts once it lasts 50 ms. Writes <out>/laps.csv and <out>/summary.json.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bbw.h"
#include "control.h"
#include "vehicle.h"

#define NCLS (DIS_SHADOW_MISSING + 1)
#define MIN_EPISODE 5   /* 50 ms */

static uint64_t rng_state;
static double urand(void) {                 /* xorshift64*: reproducible with --seed */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (double)((rng_state * 2685821657736338717ULL) >> 11) / 9007199254740992.0;
}
static double uni(double a, double b) { return a + (b - a) * urand(); }

int main(int argc, char **argv) {
    const char *lap_path = bbw_arg_str(argc, argv, "--lap", "data/lap_a.csv");
    const char *out = bbw_arg_str(argc, argv, "--out", "out/sweep");
    int runs = bbw_arg_int(argc, argv, "--runs", 200);
    int laps = bbw_arg_int(argc, argv, "--laps", 3);
    const char *cand = bbw_arg_str(argc, argv, "--candidate", "v2");
    rng_state = (uint64_t)bbw_arg_int(argc, argv, "--seed", 1) * 0x9E3779B97F4A7C15ULL + 1;

    size_t n;
    lap_row_t *lap = vehicle_load_lap(lap_path, &n);

    char path[512];
    snprintf(path, sizeof path, "mkdir -p '%s'", out);
    if (system(path) != 0) { fprintf(stderr, "cannot create %s\n", out); return 1; }
    snprintf(path, sizeof path, "%s/laps.csv", out);
    FILE *csv = fopen(path, "w");
    if (!csv) { perror(path); return 1; }
    fprintf(csv, "run,lap,soc0,ambient_c,grip,frames");
    for (int c = 0; c < NCLS; c++) fprintf(csv, ",f_%s", dis_name(c));
    for (int c = 0; c < NCLS; c++) fprintf(csv, ",ep_%s", dis_name(c));
    fprintf(csv, ",ep_lock_wings_closing,ep_lock_low_grip,worst_lock_excess_pct,max_motor_c,v1_harvest_mj,v2_safe_harvest_mj\n");

    uint64_t tot_frames = 0, tot_f[NCLS] = {0}, tot_ep[NCLS] = {0};
    int laps_unsafe = 0, laps_lock = 0, total_laps = 0, laps_lock_wings = 0, laps_lock_grip = 0, laps_motor = 0;
    double sum_v1 = 0, sum_v2safe = 0;

    for (int run = 0; run < runs; run++) {
        double soc0 = uni(0.50, 0.92), amb = uni(10, 45), grip = uni(0.85, 1.05);
        vehicle_t car;
        vehicle_init(&car, soc0, amb, grip);
        for (int L = 0; L < laps; L++) {
            vehicle_new_lap(&car);
            uint64_t fcount[NCLS] = {0}, ep[NCLS] = {0};
            int prev = DIS_NONE, runlen = 0, lock_wings = 0, lock_grip = 0;
            double wing_at_start = 1;
            double worst = 0, max_motor = car.motor_temp_c, h1 = 0, h2 = 0;
            for (size_t i = 0; i < n; i++) {
                bbw_frame_t f;
                vehicle_frame(&car, &lap[i], &lap[i + 1 < n ? i + 1 : i], (uint64_t)i, &f);
                bbw_cmd_t c1, c2;
                strategy_v1(&f, &c1);
                strategy_candidate(cand, &f, &c2);
                int cls = classify(&f, &c1, &c2, 1, 50.0);
                fcount[cls]++;
                /* an episode counts once it lasts MIN_EPISODE frames (same rule as the report) */
                runlen = cls == prev ? runlen + 1 : 1;
                if (runlen == 1) wing_at_start = f.wing_closed;
                if (runlen == MIN_EPISODE && cls != DIS_NONE && cls != DIS_WITHIN_LIMITS) {
                    ep[cls]++;
                    /* why did the rear lock? wings still closing, or simply too little grip */
                    if (cls == DIS_REAR_LOCK_RISK) { if (wing_at_start < 0.99) lock_wings++; else lock_grip++; }
                }
                prev = cls;
                double rear2 = c2.f_regen + c2.f_friction;
                if (f.f_rear_grip > 0 && rear2 / f.f_rear_grip - 1 > worst) worst = rear2 / f.f_rear_grip - 1;
                double p1 = 0;
                vehicle_apply(&car, &f, &c1, &p1);       /* v1 drives the car */
                h1 += p1 * BBW_DT;
                /* energy v2 would have harvested where its choice was safe */
                h2 += (cls == DIS_NONE || cls == DIS_WITHIN_LIMITS) ? c2.f_regen * f.v * BBW_DT : p1 * BBW_DT;
                if (car.motor_temp_c > max_motor) max_motor = car.motor_temp_c;
            }
            int unsafe = ep[DIS_REAR_LOCK_RISK] + ep[DIS_OVER_BATTERY] + ep[DIS_OVER_LAP_CAP] + ep[DIS_OVER_MOTOR] > 0;
            laps_unsafe += unsafe;
            laps_lock += ep[DIS_REAR_LOCK_RISK] > 0;
            total_laps++;
            sum_v1 += h1;
            sum_v2safe += h2;
            tot_frames += n;
            fprintf(csv, "%d,%d,%.3f,%.1f,%.3f,%zu", run, L + 1, soc0, amb, grip, n);
            for (int c = 0; c < NCLS; c++) { fprintf(csv, ",%llu", (unsigned long long)fcount[c]); tot_f[c] += fcount[c]; }
            for (int c = 0; c < NCLS; c++) { fprintf(csv, ",%llu", (unsigned long long)ep[c]); tot_ep[c] += ep[c]; }
            fprintf(csv, ",%d,%d,%.1f,%.1f,%.3f,%.3f\n", lock_wings, lock_grip, worst * 100, max_motor, h1 / 1e6, h2 / 1e6);
            laps_lock_wings += lock_wings > 0;
            laps_lock_grip += lock_grip > 0;
            laps_motor += ep[DIS_OVER_MOTOR] > 0;
        }
    }
    fclose(csv);

    snprintf(path, sizeof path, "%s/summary.json", out);
    FILE *js = fopen(path, "w");
    if (js) {
        fprintf(js, "{\n  \"candidate\": \"%s\",\n  \"runs\": %d,\n  \"laps\": %d,\n  \"frames\": %llu,\n", cand, runs, total_laps,
                (unsigned long long)tot_frames);
        fprintf(js, "  \"laps_with_unsafe_v2\": %d,\n  \"laps_with_rear_lock_risk\": %d,\n", laps_unsafe, laps_lock);
        fprintf(js, "  \"laps_with_lock_while_wings_close\": %d,\n  \"laps_with_lock_low_grip\": %d,\n"
                    "  \"laps_with_hot_motor_overreach\": %d,\n", laps_lock_wings, laps_lock_grip, laps_motor);
        fprintf(js, "  \"v1_harvest_mj_per_lap\": %.3f,\n  \"v2_safe_harvest_mj_per_lap\": %.3f,\n",
                sum_v1 / 1e6 / total_laps, sum_v2safe / 1e6 / total_laps);
        fprintf(js, "  \"frames_by_class\": {");
        for (int c = 0; c < NCLS; c++) fprintf(js, "%s\"%s\": %llu", c ? ", " : "", dis_name(c), (unsigned long long)tot_f[c]);
        fprintf(js, "},\n  \"episodes_by_class\": {");
        for (int c = 0; c < NCLS; c++) fprintf(js, "%s\"%s\": %llu", c ? ", " : "", dis_name(c), (unsigned long long)tot_ep[c]);
        fprintf(js, "}\n}\n");
        fclose(js);
    }
    printf("[%s] swept %d laps (%d runs x %d): unsafe on %d laps (%.0f%%), rear lock risk on %d laps; "
           "harvest per lap v1 %.2f MJ, v2 where safe %.2f MJ\n",
           cand, total_laps, runs, laps, laps_unsafe, 100.0 * laps_unsafe / total_laps, laps_lock,
           sum_v1 / 1e6 / total_laps, sum_v2safe / 1e6 / total_laps);
    free(lap);
    return 0;
}
