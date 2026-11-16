/*
 * Car model limits and the two brake strategies.
 * Every number here is illustrative and chosen to make the demo readable.
 */
#ifndef BBW_CONTROL_H
#define BBW_CONTROL_H

#include "bbw.h"

/* ---------- illustrative car parameters ---------- */
#define CAR_MASS_KG          800.0
#define CAR_G                9.81
#define CAR_REAR_BRAKE_SHARE 0.42     /* share of braking done by the rear axle */
#define CAR_MU               1.6      /* tyre friction coefficient */
#define CAR_DOWNFORCE_K      2.0      /* downforce N per (m/s)^2, whole car */
#define MGU_MAX_W            350e3    /* motor power limit */
#define BATT_USABLE_J        4.0e6    /* usable battery window */
#define BATT_CHARGE_EFF      0.92
#define LAP_HARVEST_CAP_J    2.4e6    /* per-lap braking-harvest limit (illustrative, scaled
                                         because this model only harvests under braking) */
#define DEPLOY_W             60e3     /* average motor deployment on full throttle (smoothed) */
#define DEPLOY_MIN_SOC       0.25     /* motor stops helping below this */

/* battery charge acceptance: full power below taper_start, zero at 0.98 */
double car_accept_frac(double soc, double taper_start);

/* rear grip limit at speed v */
double car_rear_grip(double v);

/* the real regen power limit right now (what the plant enforces) */
double car_regen_power_limit(double soc, double lap_harvest_j);

/* ---------- strategies ---------- */
/* v1: the trusted controller. Respects every limit. */
void   strategy_v1(const bbw_frame_t *f, bbw_cmd_t *out);

/* v2: the candidate. Harvests harder: moves more braking to the rear,
 * charges harder near a full battery, and (its bug) forgets the lap cap. */
void   strategy_v2(const bbw_frame_t *f, bbw_cmd_t *out);

/* ---------- disagreement classes ---------- */
enum {
    DIS_NONE = 0,          /* within tolerance */
    DIS_WITHIN_LIMITS,     /* different but physically allowed */
    DIS_OVER_BATTERY,      /* asked for more charge power than the battery accepts */
    DIS_OVER_LAP_CAP,      /* asked to harvest after the lap limit was used up */
    DIS_REAR_LOCK_RISK,    /* asked for more rear braking than the tyres can give */
    DIS_SHADOW_MISSING     /* v2 gave no answer for this frame */
};
const char *dis_name(int c);

/* classify v2's proposal against the real limits in frame f */
int classify(const bbw_frame_t *f, const bbw_cmd_t *v1, const bbw_cmd_t *v2,
             int v2_present, double tol_n);

#endif
