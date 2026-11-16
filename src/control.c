#include <math.h>
#include "control.h"

static double clamp(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

double car_accept_frac(double soc, double taper_start) {
    const double full = 0.98;
    if (soc <= taper_start) return 1.0;
    if (soc >= full) return 0.0;
    return (full - soc) / (full - taper_start);
}

double car_rear_grip(double v) {
    double load = CAR_MASS_KG * CAR_G + CAR_DOWNFORCE_K * v * v;
    return CAR_MU * CAR_REAR_BRAKE_SHARE * load;
}

double car_regen_power_limit(double soc, double lap_harvest_j) {
    double p = MGU_MAX_W * car_accept_frac(soc, 0.80);
    double cap_left = LAP_HARVEST_CAP_J - lap_harvest_j;
    if (cap_left < 0) cap_left = 0;
    double p_cap = cap_left / BBW_DT;
    return p < p_cap ? p : p_cap;
}

void strategy_v1(const bbw_frame_t *f, bbw_cmd_t *out) {
    out->seq = f->seq;
    out->f_regen = 0;
    out->f_friction = 0;
    if (!f->brake || f->a_req <= 0) return;

    /* never ask the rear tyres for more than 95% of their grip */
    double rear = f->f_rear_req;
    double grip = 0.95 * f->f_rear_grip;
    if (rear > grip) rear = grip;

    double p_lim = car_regen_power_limit(f->soc, f->lap_harvest_j);
    double f_reg_max = f->v > 1.0 ? p_lim / f->v : 0.0;
    out->f_regen = clamp(rear, 0, f_reg_max);
    out->f_friction = rear - out->f_regen;
}

void strategy_v2(const bbw_frame_t *f, bbw_cmd_t *out) {
    out->seq = f->seq;
    out->f_regen = 0;
    out->f_friction = 0;
    if (!f->brake || f->a_req <= 0) return;

    /* idea: shift 12% more braking to the rear so the motor can harvest more */
    double rear = f->f_rear_req * 1.12;

    /* charges at full power until 90% (v1 starts easing off at 80%) */
    double p_lim = MGU_MAX_W * car_accept_frac(f->soc, 0.90);
    /* BUG on purpose: no lap-cap check, no grip check */
    double f_reg_max = f->v > 1.0 ? p_lim / f->v : 0.0;
    out->f_regen = clamp(rear, 0, f_reg_max);
    out->f_friction = rear - out->f_regen;
}

const char *dis_name(int c) {
    switch (c) {
    case DIS_NONE:           return "agree";
    case DIS_WITHIN_LIMITS:  return "within_limits";
    case DIS_OVER_BATTERY:   return "over_battery";
    case DIS_OVER_LAP_CAP:   return "over_lap_cap";
    case DIS_REAR_LOCK_RISK: return "rear_lock_risk";
    case DIS_SHADOW_MISSING: return "shadow_missing";
    }
    return "unknown";
}

int classify(const bbw_frame_t *f, const bbw_cmd_t *v1, const bbw_cmd_t *v2,
             int v2_present, double tol_n) {
    if (!v2_present) return DIS_SHADOW_MISSING;
    double d_reg = fabs(v2->f_regen - v1->f_regen);
    double d_fri = fabs(v2->f_friction - v1->f_friction);
    if (d_reg <= tol_n && d_fri <= tol_n) return DIS_NONE;

    /* most serious problem first */
    double rear_total = v2->f_regen + v2->f_friction;
    if (rear_total > f->f_rear_grip) return DIS_REAR_LOCK_RISK;

    double p2 = v2->f_regen * f->v;
    if (f->lap_harvest_j >= LAP_HARVEST_CAP_J && p2 > 1.0) return DIS_OVER_LAP_CAP;
    double cap_left = LAP_HARVEST_CAP_J - f->lap_harvest_j;
    if (cap_left < 0) cap_left = 0;
    if (p2 * BBW_DT > cap_left + 1.0) return DIS_OVER_LAP_CAP;

    double p_batt = MGU_MAX_W * car_accept_frac(f->soc, 0.80);
    if (p2 > p_batt * 1.01 + 1.0) return DIS_OVER_BATTERY;

    return DIS_WITHIN_LIMITS;
}
