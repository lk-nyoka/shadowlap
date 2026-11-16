/*
 * The simulated car, shared by the live plant (bbw_plant) and the fast
 * multi-lap sweep (bbw_sweep), so both use exactly the same physics.
 */
#ifndef BBW_VEHICLE_H
#define BBW_VEHICLE_H

#include <stddef.h>
#include "bbw.h"

typedef struct { double t, dist, v, thr; int brake, zone; } lap_row_t;

typedef struct {
    double soc;            /* battery, 0..1 of usable window */
    double lap_harvest_j;  /* harvested so far this lap */
    double motor_temp_c;
    double wing_closed;    /* 0 straight-line mode .. 1 corner mode */
    double ambient_c;      /* air temperature */
    double mu_scale;       /* track grip factor (1 = nominal) */
} vehicle_t;

/* reads t,dist_m,speed_kmh,throttle,brake,zone; exits on error */
lap_row_t *vehicle_load_lap(const char *path, size_t *n_out);

void vehicle_init(vehicle_t *s, double soc0, double ambient_c, double mu_scale);
void vehicle_new_lap(vehicle_t *s);

/* build the frame for lap row r (nx = next row, for the braking demand) */
void vehicle_frame(const vehicle_t *s, const lap_row_t *r, const lap_row_t *nx,
                   uint64_t seq, bbw_frame_t *f);

/* apply a command for one 10 ms step: enforce physics (clamping the command
 * if needed), update battery, motor temperature and wings.
 * Returns 1 if the command had to be clamped. *p_out = regen power applied. */
int vehicle_apply(vehicle_t *s, const bbw_frame_t *f, bbw_cmd_t *c, double *p_out);

#endif
