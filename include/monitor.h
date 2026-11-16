#ifndef BBW_MONITOR_H
#define BBW_MONITOR_H
/*
 * The safety monitor: the limit checks a candidate is judged by.
 *
 * Kept apart from the controllers (src/control.c) on purpose. It has its own
 * copy of every limit, written from the specification below rather than
 * calling the controllers' or the car model's helper functions, so a bug in
 * those cannot quietly make the checks agree with it. tests/test_control.c
 * cross-checks the two copies. Its source hash is pinned in tools/monitor.sha256
 * and the gate refuses evidence produced by a different monitor.
 *
 * What it still shares with the car model: the rear grip and braking request
 * in each frame come from the plant (the model computes them).
 *
 * Limit specification:
 *   rear grip          rear braking <= frame's rear grip                (hard)
 *   braking demand     rear braking >= min(request, 95% grip) - max(3%, 150 N)  (hard)
 *   battery            charge power <= 350 kW x acceptance; acceptance 1 up to
 *                      80% charge, falling linearly to 0 at 98%           (soft)
 *   lap energy         no harvesting beyond 2.4 MJ per lap                (soft)
 *   motor              motor braking <= min(9 kN, 350 kW / speed), derated
 *                      linearly from 130 C to zero at 160 C               (soft)
 */
#include "bbw.h"

enum {
    DIS_NONE = 0,          /* within tolerance of v1 and inside every limit */
    DIS_WITHIN_LIMITS,     /* different from v1 but inside every limit */
    DIS_OVER_BATTERY,      /* soft: more charge power than the battery accepts */
    DIS_OVER_LAP_CAP,      /* soft: harvesting after the lap limit was used up */
    DIS_REAR_LOCK_RISK,    /* HARD: more rear braking than the tyres can give */
    DIS_OVER_MOTOR,        /* soft: more braking than a hot motor can safely give */
    DIS_DEMAND_NOT_MET,    /* HARD: less rear braking than asked, though grip allowed it */
    DIS_SHADOW_MISSING,    /* no valid answer for this frame (keep last) */
    DIS_N
};

#define MON_MASS_KG        800.0
#define MON_MGU_W          350e3
#define MON_LAP_CAP_J      2.4e6
#define MON_MOTOR_MAX_N    9000.0
#define MON_DERATE_FROM_C  130.0
#define MON_DERATE_TO_C    160.0
#define MON_TAPER_FROM     0.80
#define MON_TAPER_TO       0.98
#define MON_GRIP_TARGET    0.95
#define MON_DEMAND_TOL_N   150.0
#define MON_DEMAND_TOL_FR  0.03

const char *dis_name(int c);
int    dis_is_hard(int c);
int    hazard(const bbw_frame_t *f, const bbw_cmd_t *c);
double grip_use(const bbw_frame_t *f, const bbw_cmd_t *c);
int    cmd_plausible(const bbw_cmd_t *c);
int    classify(const bbw_frame_t *f, const bbw_cmd_t *v1, const bbw_cmd_t *v2,
                int v2_present, double tol_n);

/* the monitor's own limits, exposed for the cross-check test */
double mon_battery_power_limit(double soc);
double mon_motor_force_limit(double v, double temp_c);

/* Consequence of under-braking: how much faster the car would still be going
 * at the end of a braking zone, from braking it did not get (rear shortfall
 * against min(request, 95% grip), applied to the whole car's mass). Speed in
 * the replay does not change; this is an estimate of what would have. */
typedef struct {
    double dv;          /* m/s still owed in this braking zone */
    double extra_m;     /* extra distance travelled in this zone */
    double worst_kmh;   /* worst extra speed at any point so far */
    double worst_m;     /* worst extra distance in a zone so far */
} mon_shortfall_t;
void   mon_shortfall_update(mon_shortfall_t *s, const bbw_frame_t *f, const bbw_cmd_t *c);

#endif
