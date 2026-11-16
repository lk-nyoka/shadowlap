/*
 * bbw_comparer - lines up what v1 did with what v2 would have done.
 *
 * Reads plant records (what actually happened) and v2's proposals, joins
 * them by frame number, classifies every disagreement and writes:
 *   <out>/compare.csv   one row per cycle
 *   <out>/summary.json  totals and v1 timing
 * If v2 has not answered a frame within 1 second, the frame is logged as
 * "shadow_missing" (v2 crashed, froze or fell behind).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bbw.h"
#include "control.h"

#define TAB_N 8192u
#define WAIT_CYCLES 100   /* how long to wait for v2's answer (1 s) */
#define TOL_N 50.0        /* forces closer than this count as agreement */

typedef struct { int have; bbw_cmd_t c; } slot_t;

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    const char *out = bbw_arg_str(argc, argv, "--out", "out/run");
    int cpu = bbw_arg_int(argc, argv, "--cpu", -1);

    char path[512];
    snprintf(path, sizeof path, "mkdir -p '%s'", out);
    if (system(path) != 0) { fprintf(stderr, "[comparer] cannot create %s\n", out); return 1; }

    bbw_rec_t *rec = bbw_shm_open(BBW_SHM_REC, sizeof *rec, 10000);
    if (!rec) { fprintf(stderr, "[comparer] cannot open %s: %s\n", BBW_SHM_REC, strerror(errno)); return 1; }
    bbw_shadow_t *sh = bbw_shm_open(BBW_SHM_SHADOW, sizeof *sh, 10000);
    if (!sh) { fprintf(stderr, "[comparer] cannot open %s: %s\n", BBW_SHM_SHADOW, strerror(errno)); return 1; }
    bbw_rt_setup("comparer", BBW_PRIO_COMPARER, cpu);

    snprintf(path, sizeof path, "%s/compare.csv", out);
    FILE *csv = fopen(path, "w");
    if (!csv) { perror(path); return 1; }
    fprintf(csv, "seq,t_lap,dist_m,zone,v_kmh,brake,a_req,soc,lap_harvest_mj,f_rear_req,f_rear_grip,"
                 "v1_regen,v1_friction,v1_applied,v1_clamped,v1_latency_us,p_regen_kw,"
                 "v2_present,v2_regen,v2_friction,delta_regen,class,shadow_dropped,plant_lag_us,motor_temp_c,wing_closed\n");

    slot_t *tab = calloc(TAB_N, sizeof *tab);
    bbw_record_t *pend = malloc(TAB_N * sizeof *pend);
    uint64_t ph = 0, pt = 0;           /* pending queue head/tail */
    uint64_t newest = 0;

    size_t lat_cap = 1 << 16, lat_n = 0;
    int64_t *lat = malloc(lat_cap * sizeof *lat);
    uint64_t rows = 0, misses = 0, clamps = 0, overruns = 0, count[8] = {0};
    uint64_t last_dropped = 0;
    fprintf(stderr, "[comparer] writing %s\n", path);

    for (;;) {
        int busy = 0;
        /* read the flag first: everything published before it is drained below */
        int done = atomic_load(&rec->run_done);
        bbw_record_t r;
        while (ph - pt < TAB_N && rec_ring_pop(&rec->cycles, &r)) {
            pend[ph++ & (TAB_N - 1)] = r;
            if (r.f.seq > newest) newest = r.f.seq;
            busy = 1;
        }
        bbw_cmd_t c;
        while (cmd_ring_pop(&sh->out, &c)) {
            slot_t *s = &tab[c.seq & (TAB_N - 1)];
            s->have = 1;
            s->c = c;
            busy = 1;
        }

        while (pt < ph) {
            bbw_record_t *p = &pend[pt & (TAB_N - 1)];
            slot_t *s = &tab[p->f.seq & (TAB_N - 1)];
            int present = s->have && s->c.seq == p->f.seq;
            if (!present && !done && newest - p->f.seq < WAIT_CYCLES) break;   /* keep waiting */

            int cls = classify(&p->f, &p->cmd, &s->c, present, TOL_N);
            count[cls]++;
            if (!p->applied) misses++;
            if (p->clamped) clamps++;
            if (p->plant_lag_ns > BBW_PLANT_LATE_NS) overruns++;
            if (p->latency_ns >= 0) {
                if (lat_n == lat_cap) { lat_cap *= 2; lat = realloc(lat, lat_cap * sizeof *lat); }
                lat[lat_n++] = p->latency_ns;
            }
            last_dropped = p->shadow_dropped;
            fprintf(csv, "%llu,%.3f,%.1f,%d,%.2f,%d,%.3f,%.4f,%.4f,%.1f,%.1f,"
                         "%.1f,%.1f,%d,%d,%.1f,%.2f,%d,%.1f,%.1f,%.1f,%s,%llu,%.1f,%.1f,%.2f\n",
                    (unsigned long long)p->f.seq, p->f.t_lap, p->f.dist_m, p->f.zone, p->f.v * 3.6,
                    p->f.brake, p->f.a_req, p->f.soc, p->f.lap_harvest_j / 1e6,
                    p->f.f_rear_req, p->f.f_rear_grip,
                    p->cmd.f_regen, p->cmd.f_friction, p->applied, p->clamped,
                    p->latency_ns >= 0 ? (double)p->latency_ns / 1000.0 : -1.0, p->p_regen_w / 1000.0,
                    present, present ? s->c.f_regen : 0.0, present ? s->c.f_friction : 0.0,
                    present ? s->c.f_regen - p->cmd.f_regen : 0.0, dis_name(cls),
                    (unsigned long long)p->shadow_dropped, (double)p->plant_lag_ns / 1000.0,
                    p->f.motor_temp_c, p->f.wing_closed);
            s->have = 0;
            pt++;
            rows++;
            busy = 1;
        }
        fflush(csv);                       /* lets tools/live.py show rows as they happen */

        if (done && pt == ph && !busy) break;
        if (!busy) usleep(2000);
    }
    fclose(csv);

    qsort(lat, lat_n, sizeof *lat, cmp_i64);
#define PCT(q) (lat_n ? (double)lat[(size_t)fmin((double)lat_n - 1, floor((q) * (double)lat_n))] / 1000.0 : -1.0)
    snprintf(path, sizeof path, "%s/summary.json", out);
    FILE *js = fopen(path, "w");
    if (js) {
        fprintf(js, "{\n  \"cycles\": %llu,\n  \"v1_deadline_misses\": %llu,\n  \"plant_overruns\": %llu,\n  \"v1_clamped\": %llu,\n"
                    "  \"v1_latency_us\": {\"p50\": %.1f, \"p99\": %.1f, \"p999\": %.1f, \"max\": %.1f},\n"
                    "  \"shadow_frames_dropped\": %llu,\n  \"classes\": {",
                (unsigned long long)rows, (unsigned long long)misses, (unsigned long long)overruns,
                (unsigned long long)clamps, PCT(0.50), PCT(0.99), PCT(0.999), lat_n ? (double)lat[lat_n - 1] / 1000.0 : -1.0,
                (unsigned long long)last_dropped);
        for (int i = 0; i <= DIS_SHADOW_MISSING; i++)
            fprintf(js, "%s\"%s\": %llu", i ? ", " : "", dis_name(i), (unsigned long long)count[i]);
        fprintf(js, "}\n}\n");
        fclose(js);
    }
    fprintf(stderr, "[comparer] done: %llu rows, v1 misses %llu, plant overruns %llu, v1 latency p99 %.1f us, max %.1f us\n",
            (unsigned long long)rows, (unsigned long long)misses, (unsigned long long)overruns, PCT(0.99),
            lat_n ? (double)lat[lat_n - 1] / 1000.0 : -1.0);
    free(lat);
    free(pend);
    free(tab);
    return 0;
}
