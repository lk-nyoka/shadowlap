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
#define CAR_MU               1.8      /* tyre friction coefficient (fitted to a real pole lap) */
#define CAR_DOWNFORCE_K      4.0      /* downforce N per (m/s)^2, whole car (fitted to a real pole lap) */
#define MGU_MAX_W            350e3    /* motor power limit */
#define BATT_USABLE_J        4.0e6    /* usable battery window */
#define BATT_CHARGE_EFF      0.92
#define LAP_HARVEST_CAP_J    2.4e6    /* per-lap braking-harvest limit (illustrative, scaled
                                         because this model only harvests under braking) */
#define DEPLOY_W             60e3     /* average motor deployment on full throttle (smoothed) */
#define DEPLOY_MIN_SOC       0.25     /* motor stops helping below this */

/* motor: constant torque at low speed, then constant power (all illustrative) */
#define MOTOR_FORCE_MAX_N    9000.0   /* most braking force the motor can make at the rear */
#define MOTOR_DERATE_START_C 130.0    /* motor starts limiting itself above this */
#define MOTOR_MAX_C          160.0    /* no motor braking at all at this temperature */
#define MOTOR_LOSS_FRAC      0.06     /* share of motor power that becomes heat */
#define MOTOR_HEAT_CAP_JK    3000.0   /* heat capacity, J per deg C */
#define MOTOR_COOL_TAU_S     90.0     /* cooling time constant */

/* aerodynamics: F1 2026-style movable wings */
#define AERO_STRAIGHT_FRAC   0.70     /* downforce left in straight-line mode */
#define WING_CLOSE_RATE      5.0      /* per second: about 0.2 s to close fully */
#define WING_OPEN_RATE       3.3      /* per second */

/* battery charge acceptance: full power below taper_start, zero at 0.98 */
double car_accept_frac(double soc, double taper_start);

/* rear grip limit at speed v, full downforce, nominal track */
double car_rear_grip(double v);

/* rear grip with the wings partly open (0..1 closed) and a track grip factor */
double car_rear_grip_aero(double v, double wing_closed, double mu_scale);

/* most braking force the motor can make at speed v and temperature temp_c;
 * use_temp = 0 ignores the temperature limit (v2's mistake) */
double car_motor_force_limit(double v, double temp_c, int use_temp);

/* the real regen power limit right now (what the plant enforces) */
double car_regen_power_limit(double soc, double lap_harvest_j);

/* ---------- strategies ---------- */
/* v1: the trusted controller. Respects every limit. */
void   strategy_v1(const bbw_frame_t *f, bbw_cmd_t *out);

/* v2: the candidate. Harvests harder: moves more braking to the rear,
 * charges harder near a full battery, and (its bugs) forgets the lap cap,
 * the grip limit and the motor's temperature limit. */
void   strategy_v2(const bbw_frame_t *f, bbw_cmd_t *out);

/* v2.1: v2 with the flaws the shadow evidence found fixed (grip, lap cap,
 * motor temperature), keeping its extra rear harvesting where it is safe. */
void   strategy_v2_fixed(const bbw_frame_t *f, bbw_cmd_t *out);

/* v3: a different idea, smoother motor/disc blending. Its bug (on purpose):
 * when the motor is not allowed to take its share (battery nearly full, or the
 * lap's energy limit used up), it does not make up the difference with the
 * discs, so the car brakes less than the driver asked. The same kind of
 * failure as braking that fades when regenerative braking is unavailable. */
void   strategy_v3(const bbw_frame_t *f, bbw_cmd_t *out);

/* pick the candidate by name: "v2" (default), "v2.1" or "v3"; NULL/unknown -> v2 */
void   strategy_candidate(const char *name, const bbw_frame_t *f, bbw_cmd_t *out);
int    candidate_known(const char *name);

#include "monitor.h"   /* the limit checks live in their own module */

#endif
