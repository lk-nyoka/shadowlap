/* The safety monitor. See include/monitor.h for the limit specification. */
#include <math.h>
#include "monitor.h"

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

double mon_battery_power_limit(double soc) {
    double acc = soc <= MON_TAPER_FROM ? 1.0 : soc >= MON_TAPER_TO ? 0.0
               : (MON_TAPER_TO - soc) / (MON_TAPER_TO - MON_TAPER_FROM);
    return MON_MGU_W * acc;
}

double mon_motor_force_limit(double v, double temp_c) {
    double lim = MON_MOTOR_MAX_N;
    if (v > 1.0 && MON_MGU_W / v < lim) lim = MON_MGU_W / v;
    if (temp_c >= MON_DERATE_TO_C) return 0.0;
    if (temp_c > MON_DERATE_FROM_C) lim *= (MON_DERATE_TO_C - temp_c) / (MON_DERATE_TO_C - MON_DERATE_FROM_C);
    return lim;
}

double grip_use(const bbw_frame_t *f, const bbw_cmd_t *c) {
    return f->f_rear_grip > 0 ? (c->f_regen + c->f_friction) / f->f_rear_grip : 0;
}

int cmd_plausible(const bbw_cmd_t *c) {
    return isfinite(c->f_regen) && isfinite(c->f_friction) &&
           c->f_regen >= -1.0 && c->f_friction >= -1.0 &&
           c->f_regen <= MON_MOTOR_MAX_N * 1.5 && c->f_friction <= 100000.0;
}

static double demand_target(const bbw_frame_t *f) {
    double cap = MON_GRIP_TARGET * f->f_rear_grip;
    return f->f_rear_req < cap ? f->f_rear_req : cap;
}

int hazard(const bbw_frame_t *f, const bbw_cmd_t *c) {
    double rear = c->f_regen + c->f_friction;
    /* hard limits, most serious first */
    if (rear > f->f_rear_grip) return DIS_REAR_LOCK_RISK;
    if (f->brake && f->a_req > 0) {
        double want = demand_target(f);
        double tol = want * MON_DEMAND_TOL_FR > MON_DEMAND_TOL_N ? want * MON_DEMAND_TOL_FR : MON_DEMAND_TOL_N;
        if (rear < want - tol) return DIS_DEMAND_NOT_MET;
    }
    /* soft limits */
    double p = c->f_regen * f->v;
    double cap_left = MON_LAP_CAP_J - f->lap_harvest_j;
    if (cap_left < 0) cap_left = 0;
    if ((f->lap_harvest_j >= MON_LAP_CAP_J && p > 1.0) || p * BBW_DT > cap_left + 1.0) return DIS_OVER_LAP_CAP;
    if (p > mon_battery_power_limit(f->soc) * 1.01 + 1.0) return DIS_OVER_BATTERY;
    if (c->f_regen > mon_motor_force_limit(f->v, f->motor_temp_c) * 1.01 + 1.0) return DIS_OVER_MOTOR;
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

void mon_shortfall_update(mon_shortfall_t *s, const bbw_frame_t *f, const bbw_cmd_t *c) {
    if (!f->brake || f->a_req <= 0) {           /* braking zone over: close it */
        s->dv = 0;
        s->extra_m = 0;
        return;
    }
    double short_n = demand_target(f) - (c->f_regen + c->f_friction);
    if (short_n > 0) s->dv += short_n / MON_MASS_KG * BBW_DT;
    s->extra_m += s->dv * BBW_DT;
    if (s->dv * 3.6 > s->worst_kmh) s->worst_kmh = s->dv * 3.6;
    if (s->extra_m > s->worst_m) s->worst_m = s->extra_m;
}
