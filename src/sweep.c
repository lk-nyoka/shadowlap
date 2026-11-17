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
 * track grip 85-105% of nominal. Hard limits (rear lock, braking demand not
 * met) count from a single 10 ms frame; soft limits (battery, lap energy,
 * motor heat) once they last 50 ms. The same checks run on v1.
 * --mode counterfactual lets v2 drive its own copy of the car.
 * Writes <out>/laps.csv and <out>/summary.json.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bbw.h"
#include "control.h"
#include "vehicle.h"

#define NCLS DIS_N
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
    int seed = bbw_arg_int(argc, argv, "--seed", 1);
    const char *cand = bbw_arg_str(argc, argv, "--candidate", "v2");
    /* shadow: v2 sees the car v1 drives (what a car in shadow mode really sees).
     * counterfactual: v2 also drives its own copy of the car, so its battery,
     * lap energy and motor heat follow its OWN decisions. */
    const char *mode = bbw_arg_str(argc, argv, "--mode", "shadow");
    int cf = strcmp(mode, "counterfactual") == 0;
    if (!cf && strcmp(mode, "shadow") != 0) { fprintf(stderr, "--mode must be shadow or counterfactual\n"); return 1; }
    if (!candidate_known(cand)) { fprintf(stderr, "unknown candidate %s\n", cand); return 1; }
    if (runs < 1 || laps < 1) { fprintf(stderr, "--runs and --laps must be >= 1\n"); return 1; }
    rng_state = (uint64_t)seed * 0x9E3779B97F4A7C15ULL + 1;

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
    fprintf(csv, ",ep_lock_wings_closing,ep_lock_low_grip,worst_lock_excess_pct,peak_grip_use_v2,max_motor_c,"
                 "v1_harvest_mj,v2_safe_harvest_mj,v1_limit_frames,v2_extra_kmh,v2_extra_m\n");

    uint64_t tot_frames = 0, tot_f[NCLS] = {0}, tot_ep[NCLS] = {0}, v1_hz[NCLS] = {0}, v1_hz_total = 0;
    int laps_unsafe = 0, laps_lock = 0, total_laps = 0, laps_lock_wings = 0, laps_lock_grip = 0, laps_motor = 0;
    int laps_hard = 0, laps_demand = 0;
    double sum_v1 = 0, sum_v2safe = 0, peak_v1 = 0, peak_v2 = 0, worst_ex = 0, worst_kmh = 0, worst_m = 0;
    double worst_kmh_v1 = 0;
    mon_margin_t mg1, mg2;
    mon_margin_init(&mg1);
    mon_margin_init(&mg2);
    int mg_run[5] = {0}, mg_lap[5] = {0};       /* where the candidate came closest, per limit */

    for (int run = 0; run < runs; run++) {
        double soc0 = uni(0.50, 0.92), amb = uni(10, 45), grip = uni(0.85, 1.05);
        vehicle_t car, car2;
        vehicle_init(&car, soc0, amb, grip);
        car2 = car;
        for (int L = 0; L < laps; L++) {
            vehicle_new_lap(&car);
            vehicle_new_lap(&car2);
            uint64_t fcount[NCLS] = {0}, ep[NCLS] = {0}, v1lim = 0;
            int prev = DIS_NONE, runlen = 0, lock_wings = 0, lock_grip = 0;
            double wing_at_start = 1;
            double worst = 0, pk2 = 0, max_motor = (cf ? car2 : car).motor_temp_c, h1 = 0, h2 = 0;
            mon_shortfall_t sh1 = {0}, sh2 = {0};
            for (size_t i = 0; i < n; i++) {
                const lap_row_t *nx = &lap[i + 1 < n ? i + 1 : i];
                bbw_frame_t f, f2;
                vehicle_frame(&car, &lap[i], nx, (uint64_t)i, &f);
                if (cf) vehicle_frame(&car2, &lap[i], nx, (uint64_t)i, &f2); else f2 = f;
                bbw_cmd_t c1, c2, c1b;
                strategy_v1(&f, &c1);
                strategy_candidate(cand, &f2, &c2);
                if (cf) strategy_v1(&f2, &c1b); else c1b = c1;   /* v1 in v2's situation, for "agree" */
                int cls = classify(&f2, &c1b, &c2, 1, 50.0);
                fcount[cls]++;
                mon_margin_t before = mg2;
                mon_margin_update(&mg1, &f, &c1);
                mon_margin_update(&mg2, &f2, &c2);
                if (mg2.t_grip != before.t_grip || mg2.grip != before.grip) { mg_run[0] = run; mg_lap[0] = L + 1; }
                if (mg2.battery != before.battery) { mg_run[1] = run; mg_lap[1] = L + 1; }
                if (mg2.motor != before.motor) { mg_run[2] = run; mg_lap[2] = L + 1; }
                if (mg2.lap_energy != before.lap_energy) { mg_run[3] = run; mg_lap[3] = L + 1; }
                if (mg2.demand != before.demand) { mg_run[4] = run; mg_lap[4] = L + 1; }
                mon_shortfall_update(&sh1, &f, &c1);
                mon_shortfall_update(&sh2, &f2, &c2);
                int h1z = hazard(&f, &c1);                        /* same checks on v1 */
                if (h1z != DIS_NONE) { v1_hz[h1z]++; v1_hz_total++; v1lim++; }
                /* hard limits count from one frame; soft ones once they last MIN_EPISODE */
                runlen = cls == prev ? runlen + 1 : 1;
                if (runlen == 1) wing_at_start = f2.wing_closed;
                int need = dis_is_hard(cls) ? 1 : MIN_EPISODE;
                if (runlen == need && cls != DIS_NONE && cls != DIS_WITHIN_LIMITS) {
                    ep[cls]++;
                    if (cls == DIS_REAR_LOCK_RISK) { if (wing_at_start < 0.99) lock_wings++; else lock_grip++; }
                }
                prev = cls;
                if (f2.brake) {
                    double g2 = grip_use(&f2, &c2), g1 = grip_use(&f, &c1);
                    if (g2 - 1 > worst) worst = g2 - 1;
                    if (g2 > pk2) pk2 = g2;
                    if (g1 > peak_v1) peak_v1 = g1;
                }
                double p1 = 0, p2 = 0;
                bbw_cmd_t a1 = c1;
                vehicle_apply(&car, &f, &a1, &p1);                /* v1 drives the real car */
                h1 += p1 * BBW_DT;
                if (cf) {
                    bbw_cmd_t a2 = c2;
                    vehicle_apply(&car2, &f2, &a2, &p2);          /* v2 drives its own copy */
                    h2 += p2 * BBW_DT;
                } else {
                    /* energy v2 would have harvested where its choice was safe */
                    h2 += (cls == DIS_NONE || cls == DIS_WITHIN_LIMITS) ? c2.f_regen * f.v * BBW_DT : p1 * BBW_DT;
                }
                double mt = (cf ? car2 : car).motor_temp_c;
                if (mt > max_motor) max_motor = mt;
            }
            int hard = ep[DIS_REAR_LOCK_RISK] + ep[DIS_DEMAND_NOT_MET] > 0;
            int unsafe = hard || ep[DIS_OVER_BATTERY] + ep[DIS_OVER_LAP_CAP] + ep[DIS_OVER_MOTOR] > 0;
            laps_unsafe += unsafe;
            laps_hard += hard;
            laps_lock += ep[DIS_REAR_LOCK_RISK] > 0;
            laps_demand += ep[DIS_DEMAND_NOT_MET] > 0;
            total_laps++;
            sum_v1 += h1;
            sum_v2safe += h2;
            tot_frames += n;
            if (pk2 > peak_v2) peak_v2 = pk2;
            if (worst > worst_ex) worst_ex = worst;
            fprintf(csv, "%d,%d,%.3f,%.1f,%.3f,%zu", run, L + 1, soc0, amb, grip, n);
            for (int c = 0; c < NCLS; c++) { fprintf(csv, ",%llu", (unsigned long long)fcount[c]); tot_f[c] += fcount[c]; }
            for (int c = 0; c < NCLS; c++) { fprintf(csv, ",%llu", (unsigned long long)ep[c]); tot_ep[c] += ep[c]; }
            fprintf(csv, ",%d,%d,%.1f,%.3f,%.1f,%.3f,%.3f,%llu,%.2f,%.2f\n", lock_wings, lock_grip, worst * 100, pk2, max_motor,
                    h1 / 1e6, h2 / 1e6, (unsigned long long)v1lim, sh2.worst_kmh, sh2.worst_m);
            if (sh2.worst_kmh > worst_kmh) worst_kmh = sh2.worst_kmh;
            if (sh2.worst_m > worst_m) worst_m = sh2.worst_m;
            if (sh1.worst_kmh > worst_kmh_v1) worst_kmh_v1 = sh1.worst_kmh;
            laps_lock_wings += lock_wings > 0;
            laps_lock_grip += lock_grip > 0;
            laps_motor += ep[DIS_OVER_MOTOR] > 0;
        }
    }
    fclose(csv);

    snprintf(path, sizeof path, "%s/summary.json", out);
    FILE *js = fopen(path, "w");
    if (js) {
        fprintf(js, "{\n  \"schema\": 2,\n  \"tool\": \"bbw_sweep\",\n  \"candidate\": \"%s\",\n  \"mode\": \"%s\",\n"
                    "  \"seed\": %d,\n  \"lap_file\": \"%s\",\n  \"lap_rows\": %zu,\n  \"runs\": %d,\n  \"laps_per_run\": %d,\n"
                    "  \"ranges\": {\"soc0\": [0.50, 0.92], \"ambient_c\": [10, 45], \"grip\": [0.85, 1.05]},\n"
                    "  \"rules\": {\"hard_limits_count_from_frames\": 1, \"soft_limits_count_from_frames\": %d},\n"
                    "  \"laps\": %d,\n  \"frames\": %llu,\n",
                cand, mode, seed, lap_path, n, runs, laps, MIN_EPISODE, total_laps, (unsigned long long)tot_frames);
        fprintf(js, "  \"laps_with_unsafe_v2\": %d,\n  \"laps_with_hard_limit_break\": %d,\n  \"laps_with_rear_lock_risk\": %d,\n"
                    "  \"laps_with_demand_not_met\": %d,\n", laps_unsafe, laps_hard, laps_lock, laps_demand);
        fprintf(js, "  \"laps_with_lock_while_wings_close\": %d,\n  \"laps_with_lock_low_grip\": %d,\n"
                    "  \"laps_with_hot_motor_overreach\": %d,\n", laps_lock_wings, laps_lock_grip, laps_motor);
        fprintf(js, "  \"peak_rear_grip_use\": {\"v1\": %.4f, \"v2\": %.4f},\n  \"worst_lock_excess_pct\": %.2f,\n",
                peak_v1, peak_v2, worst_ex * 100);
        fprintf(js, "  \"worst_extra_speed_kmh\": {\"v1\": %.2f, \"v2\": %.2f},\n  \"worst_extra_distance_m\": %.2f,\n",
                worst_kmh_v1, worst_kmh, worst_m);
        fprintf(js, "  \"v1_harvest_mj_per_lap\": %.3f,\n  \"v2_%s_harvest_mj_per_lap\": %.3f,\n  \"v2_safe_harvest_mj_per_lap\": %.3f,\n",
                sum_v1 / 1e6 / total_laps, cf ? "own" : "safe", sum_v2safe / 1e6 / total_laps, sum_v2safe / 1e6 / total_laps);
        fprintf(js, "  \"closest_approach\": {\n");
        const mon_margin_t *mm[2] = { &mg1, &mg2 };
        for (int k = 0; k < 2; k++)
            fprintf(js, "    \"%s\": {\"rear_grip\": %.4f, \"battery\": %.4f, \"motor\": %.4f, \"lap_energy\": %.4f, "
                        "\"demand_delivered\": %.4f}%s\n", k ? "v2" : "v1",
                    mm[k]->grip, mm[k]->battery, mm[k]->motor, mm[k]->lap_energy, mm[k]->demand, k ? "" : ",");
        fprintf(js, "  },\n  \"closest_approach_where\": {\"rear_grip\": [%d, %d, %.2f], \"battery\": [%d, %d, %.2f], "
                    "\"motor\": [%d, %d, %.2f], \"lap_energy\": [%d, %d, %.2f], \"demand_delivered\": [%d, %d, %.2f]},\n",
                mg_run[0], mg_lap[0], mg2.t_grip, mg_run[1], mg_lap[1], mg2.t_battery, mg_run[2], mg_lap[2], mg2.t_motor,
                mg_run[3], mg_lap[3], mg2.t_lap_energy, mg_run[4], mg_lap[4], mg2.t_demand);
        fprintf(js, "  \"v1_limit_break_frames\": %llu,\n  \"frames_by_class\": {", (unsigned long long)v1_hz_total);
        for (int c = 0; c < NCLS; c++) fprintf(js, "%s\"%s\": %llu", c ? ", " : "", dis_name(c), (unsigned long long)tot_f[c]);
        fprintf(js, "},\n  \"episodes_by_class\": {");
        for (int c = 0; c < NCLS; c++) fprintf(js, "%s\"%s\": %llu", c ? ", " : "", dis_name(c), (unsigned long long)tot_ep[c]);
        fprintf(js, "},\n  \"v1_limit_breaks\": {");
        for (int c = 0; c < NCLS; c++) fprintf(js, "%s\"%s\": %llu", c ? ", " : "", dis_name(c), (unsigned long long)v1_hz[c]);
        fprintf(js, "}\n}\n");
        fclose(js);
    }
    printf("[%s, %s] swept %d laps (%d runs x %d): unsafe on %d laps (%.0f%%), hard-limit break on %d, "
           "peak grip use v1 %.2f v2 %.2f, v1 limit breaks %llu frames\n",
           cand, mode, total_laps, runs, laps, laps_unsafe, 100.0 * laps_unsafe / total_laps, laps_hard,
           peak_v1, peak_v2, (unsigned long long)v1_hz_total);
    free(lap);
    return 0;
}
