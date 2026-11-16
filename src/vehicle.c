#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "control.h"
#include "vehicle.h"

lap_row_t *vehicle_load_lap(const char *path, size_t *n_out) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); exit(1); }
    size_t cap = 16384, n = 0;
    lap_row_t *rows = malloc(cap * sizeof *rows);
    char line[256];
    if (!rows || !fgets(line, sizeof line, fp)) { fprintf(stderr, "empty lap file\n"); exit(1); }
    while (fgets(line, sizeof line, fp)) {
        lap_row_t r;
        double kmh;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%d,%d", &r.t, &r.dist, &kmh, &r.thr, &r.brake, &r.zone) != 6)
            continue;
        r.v = kmh / 3.6;
        if (n == cap) {
            cap *= 2;
            lap_row_t *nr = realloc(rows, cap * sizeof *rows);
            if (!nr) { fprintf(stderr, "out of memory\n"); exit(1); }
            rows = nr;
        }
        rows[n++] = r;
    }
    fclose(fp);
    if (n < 10) { fprintf(stderr, "lap file has too few rows\n"); exit(1); }
    double dt = (rows[n - 1].t - rows[0].t) / (double)(n - 1);
    if (fabs(dt - BBW_DT) > 0.002)
        fprintf(stderr, "warning: lap sample spacing %.4fs, expected %.4fs\n", dt, BBW_DT);
    *n_out = n;
    return rows;
}

void vehicle_init(vehicle_t *s, double soc0, double ambient_c, double mu_scale) {
    memset(s, 0, sizeof *s);
    s->soc = soc0;
    s->ambient_c = ambient_c;
    s->motor_temp_c = ambient_c + 40.0;   /* warmed up from the out-lap */
    s->wing_closed = 1.0;
    s->mu_scale = mu_scale;
}

void vehicle_new_lap(vehicle_t *s) { s->lap_harvest_j = 0; }

void vehicle_frame(const vehicle_t *s, const lap_row_t *r, const lap_row_t *nx,
                   uint64_t seq, bbw_frame_t *f) {
    memset(f, 0, sizeof *f);
    f->seq = seq;
    f->t_lap = r->t;
    f->dist_m = r->dist;
    f->v = r->v;
    f->throttle = r->thr;
    f->brake = r->brake;
    f->zone = r->zone;
    double a = (r->v - nx->v) / BBW_DT;
    f->a_req = (r->brake && a > 0) ? (a > 60 ? 60 : a) : 0;
    f->soc = s->soc;
    f->lap_harvest_j = s->lap_harvest_j;
    f->f_rear_req = CAR_REAR_BRAKE_SHARE * CAR_MASS_KG * f->a_req;
    f->f_rear_grip = car_rear_grip_aero(r->v, s->wing_closed, s->mu_scale);
    f->motor_temp_c = s->motor_temp_c;
    f->wing_closed = s->wing_closed;
}

int vehicle_apply(vehicle_t *s, const bbw_frame_t *f, bbw_cmd_t *c, double *p_out) {
    int clamped = 0;
    /* the car enforces physics no matter what it is told */
    double p_lim = car_regen_power_limit(s->soc, s->lap_harvest_j);
    double f_lim = f->v > 1 ? p_lim / f->v : 0;
    double f_motor = car_motor_force_limit(f->v, s->motor_temp_c, 1);
    if (f_motor < f_lim) f_lim = f_motor;
    if (c->f_regen < 0 || c->f_regen > f_lim * 1.001 + 1.0) {
        double reg = c->f_regen < 0 ? 0 : f_lim;
        c->f_friction += c->f_regen - reg;
        c->f_regen = reg;
        clamped = 1;
    }
    double p = c->f_regen * f->v;

    /* battery: harvest under braking, deploy on full throttle */
    s->soc += p * BBW_DT * BATT_CHARGE_EFF / BATT_USABLE_J;
    s->lap_harvest_j += p * BBW_DT;
    double p_deploy = (f->throttle > 0.9 && s->soc > DEPLOY_MIN_SOC) ? DEPLOY_W * f->throttle : 0;
    s->soc -= p_deploy * BBW_DT / BATT_USABLE_J;
    if (s->soc > 1) s->soc = 1;
    if (s->soc < 0) s->soc = 0;

    /* motor heats with the power it handles, cools towards the air */
    s->motor_temp_c += ((p + p_deploy) * MOTOR_LOSS_FRAC / MOTOR_HEAT_CAP_JK
                        - (s->motor_temp_c - s->ambient_c) / MOTOR_COOL_TAU_S) * BBW_DT;

    /* wings: open (straight-line mode) only at full throttle above 200 km/h,
     * and they take time to move, so braking starts with less grip */
    double target = (!f->brake && f->throttle > 0.9 && f->v > 200.0 / 3.6) ? 0.0 : 1.0;
    if (s->wing_closed < target) {
        s->wing_closed += WING_CLOSE_RATE * BBW_DT;
        if (s->wing_closed > target) s->wing_closed = target;
    } else if (s->wing_closed > target) {
        s->wing_closed -= WING_OPEN_RATE * BBW_DT;
        if (s->wing_closed < target) s->wing_closed = target;
    }

    *p_out = p;
    return clamped;
}
