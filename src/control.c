#include <math.h>
#include <string.h>
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

double car_rear_grip_aero(double v, double wing_closed, double mu_scale) {
    if (wing_closed < 0) wing_closed = 0;
    if (wing_closed > 1) wing_closed = 1;
    double k = CAR_DOWNFORCE_K * (AERO_STRAIGHT_FRAC + (1.0 - AERO_STRAIGHT_FRAC) * wing_closed);
    double load = CAR_MASS_KG * CAR_G + k * v * v;
    return mu_scale * CAR_MU * CAR_REAR_BRAKE_SHARE * load;
}

double car_motor_force_limit(double v, double temp_c, int use_temp) {
    double f = MOTOR_FORCE_MAX_N;
    double by_power = v > 1.0 ? MGU_MAX_W / v : MOTOR_FORCE_MAX_N;
    if (by_power < f) f = by_power;
    if (use_temp && temp_c > MOTOR_DERATE_START_C) {
        double k = (MOTOR_MAX_C - temp_c) / (MOTOR_MAX_C - MOTOR_DERATE_START_C);
        f *= k < 0 ? 0 : k;
    }
    return f;
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
    double f_motor = car_motor_force_limit(f->v, f->motor_temp_c, 1);
    if (f_motor < f_reg_max) f_reg_max = f_motor;
    out->f_regen = clamp(rear, 0, f_reg_max);
    out->f_friction = rear - out->f_regen;
}

void strategy_v2(const bbw_frame_t *f, bbw_cmd_t *out) {
    out->seq = f->seq;
    out->f_regen = 0;
    out->f_friction = 0;
    if (!f->brake || f->a_req <= 0) return;

    /* idea: shift 6% more braking to the rear so the motor can harvest more */
    double rear = f->f_rear_req * 1.06;

    /* charges at full power until 90% (v1 starts easing off at 80%) */
    double p_lim = MGU_MAX_W * car_accept_frac(f->soc, 0.90);
    /* BUGS on purpose: no lap-cap check, no grip check, no motor temperature check */
    double f_reg_max = f->v > 1.0 ? p_lim / f->v : 0.0;
    double f_motor = car_motor_force_limit(f->v, f->motor_temp_c, 0);
    if (f_motor < f_reg_max) f_reg_max = f_motor;
    out->f_regen = clamp(rear, 0, f_reg_max);
    out->f_friction = rear - out->f_regen;
}

void strategy_v2_fixed(const bbw_frame_t *f, bbw_cmd_t *out) {
    out->seq = f->seq;
    out->f_regen = 0;
    out->f_friction = 0;
    if (!f->brake || f->a_req <= 0) return;

    /* keep v2's idea (6% more rear braking) but never past 95% of grip */
    double rear = f->f_rear_req * 1.06;
    double grip = 0.95 * f->f_rear_grip;
    if (rear > grip) rear = grip;

    /* charge harder than v1 until 85% (v1 eases off from 80%), respect the lap cap */
    double p_lim = MGU_MAX_W * car_accept_frac(f->soc, 0.85);
    double v1_lim = car_regen_power_limit(f->soc, f->lap_harvest_j);
    double cap_left = LAP_HARVEST_CAP_J - f->lap_harvest_j;
    if (cap_left < 0) cap_left = 0;
    if (p_lim > cap_left / BBW_DT) p_lim = cap_left / BBW_DT;
    /* the battery's true acceptance still applies (v1's limit) */
    if (p_lim > v1_lim) p_lim = v1_lim;
    double f_reg_max = f->v > 1.0 ? p_lim / f->v : 0.0;
    double f_motor = car_motor_force_limit(f->v, f->motor_temp_c, 1);
    if (f_motor < f_reg_max) f_reg_max = f_motor;
    out->f_regen = clamp(rear, 0, f_reg_max);
    out->f_friction = rear - out->f_regen;
}

void strategy_v3(const bbw_frame_t *f, bbw_cmd_t *out) {
    out->seq = f->seq;
    out->f_regen = 0;
    out->f_friction = 0;
    if (!f->brake || f->a_req <= 0) return;

    double rear = f->f_rear_req;
    double grip = 0.95 * f->f_rear_grip;
    if (rear > grip) rear = grip;

    /* plans the motor/disc split from what the MOTOR can do ... */
    double f_motor = car_motor_force_limit(f->v, f->motor_temp_c, 1);
    double planned_regen = rear < f_motor ? rear : f_motor;
    out->f_friction = rear - planned_regen;
    /* ... then the battery limits the motor, and (BUG) the discs are not told */
    double p_lim = car_regen_power_limit(f->soc, f->lap_harvest_j);
    double f_batt = f->v > 1.0 ? p_lim / f->v : 0.0;
    out->f_regen = planned_regen < f_batt ? planned_regen : f_batt;
}

int candidate_known(const char *name) {
    return name && (!strcmp(name, "v2") || !strcmp(name, "v2.1") || !strcmp(name, "fixed") || !strcmp(name, "v3"));
}

void strategy_candidate(const char *name, const bbw_frame_t *f, bbw_cmd_t *out) {
    if (name && (strcmp(name, "v2.1") == 0 || strcmp(name, "fixed") == 0)) strategy_v2_fixed(f, out);
    else if (name && strcmp(name, "v3") == 0) strategy_v3(f, out);
    else strategy_v2(f, out);
}

const char *dis_name(int c) {
    switch (c) {
    case DIS_NONE:           return "agree";
    case DIS_WITHIN_LIMITS:  return "within_limits";
    case DIS_OVER_BATTERY:   return "over_battery";
    case DIS_OVER_LAP_CAP:   return "over_lap_cap";
    case DIS_REAR_LOCK_RISK: return "rear_lock_risk";
    case DIS_OVER_MOTOR:     return "over_motor";
    case DIS_DEMAND_NOT_MET: return "demand_not_met";
    case DIS_SHADOW_MISSING: return "shadow_missing";
    }
    return "unknown";
}

int dis_is_hard(int c) { return c == DIS_REAR_LOCK_RISK || c == DIS_DEMAND_NOT_MET; }

double grip_use(const bbw_frame_t *f, const bbw_cmd_t *c) {
    return f->f_rear_grip > 0 ? (c->f_regen + c->f_friction) / f->f_rear_grip : 0;
}

int cmd_plausible(const bbw_cmd_t *c) {
    return isfinite(c->f_regen) && isfinite(c->f_friction) &&
           c->f_regen >= -1.0 && c->f_friction >= -1.0 &&
           c->f_regen <= MOTOR_FORCE_MAX_N * 1.5 && c->f_friction <= 100000.0;
}

#define DEMAND_TOL_N   150.0   /* rear braking may fall short by this much ... */
#define DEMAND_TOL_FR  0.03    /* ... or by 3%, whichever is larger */

int hazard(const bbw_frame_t *f, const bbw_cmd_t *c) {
    double rear = c->f_regen + c->f_friction;
    /* hard limits, most serious first */
    if (rear > f->f_rear_grip) return DIS_REAR_LOCK_RISK;
    if (f->brake && f->a_req > 0) {
        double want = f->f_rear_req < 0.95 * f->f_rear_grip ? f->f_rear_req : 0.95 * f->f_rear_grip;
        double tol = want * DEMAND_TOL_FR > DEMAND_TOL_N ? want * DEMAND_TOL_FR : DEMAND_TOL_N;
        if (rear < want - tol) return DIS_DEMAND_NOT_MET;
    }
    /* soft limits */
    double p = c->f_regen * f->v;
    double cap_left = LAP_HARVEST_CAP_J - f->lap_harvest_j;
    if (cap_left < 0) cap_left = 0;
    if ((f->lap_harvest_j >= LAP_HARVEST_CAP_J && p > 1.0) || p * BBW_DT > cap_left + 1.0) return DIS_OVER_LAP_CAP;
    double p_batt = MGU_MAX_W * car_accept_frac(f->soc, 0.80);
    if (p > p_batt * 1.01 + 1.0) return DIS_OVER_BATTERY;
    if (c->f_regen > car_motor_force_limit(f->v, f->motor_temp_c, 1) * 1.01 + 1.0) return DIS_OVER_MOTOR;
    return DIS_NONE;
}

int classify(const bbw_frame_t *f, const bbw_cmd_t *v1, const bbw_cmd_t *v2,
             int v2_present, double tol_n) {
    if (!v2_present || !cmd_plausible(v2)) return DIS_SHADOW_MISSING;
    int h = hazard(f, v2);                     /* absolute: checked even if v2 == v1 */
    if (h != DIS_NONE) return h;
    double d_reg = fabs(v2->f_regen - v1->f_regen);
    double d_fri = fabs(v2->f_friction - v1->f_friction);
    return (d_reg <= tol_n && d_fri <= tol_n) ? DIS_NONE : DIS_WITHIN_LIMITS;
}
