/* Strategy tests: v1 always respects the limits; v2 breaks them where designed to. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "control.h"

static bbw_frame_t frame(double v_kmh, double a, double soc, double harvest) {
    bbw_frame_t f;
    memset(&f, 0, sizeof f);
    f.v = v_kmh / 3.6;
    f.a_req = a;
    f.brake = 1;
    f.soc = soc;
    f.lap_harvest_j = harvest;
    f.f_rear_req = CAR_REAR_BRAKE_SHARE * CAR_MASS_KG * a;
    f.f_rear_grip = car_rear_grip(f.v);
    f.motor_temp_c = 80;
    f.wing_closed = 1;
    return f;
}

int main(void) {
    bbw_cmd_t a, b;
    int fails = 0;

    /* v1 never exceeds power, cap or grip across a grid of conditions */
    for (double v = 60; v <= 340; v += 20)
        for (double acc = 5; acc <= 50; acc += 5)
            for (double soc = 0.3; soc <= 1.0; soc += 0.05)
                for (double h = 0; h <= 3e6; h += 0.25e6) {
                    bbw_frame_t f = frame(v, acc, soc, h);
                    strategy_v1(&f, &a);
                    double p = a.f_regen * f.v;
                    if (p > car_regen_power_limit(soc, h) * 1.0001 + 1) fails++;
                    if (a.f_regen > car_motor_force_limit(f.v, f.motor_temp_c, 1) + 1e-6) fails++;
                    if (a.f_regen + a.f_friction > f.f_rear_grip) fails++;
                    if (a.f_regen < 0 || a.f_friction < -1e-6) fails++;
                    if (hazard(&f, &a) != DIS_NONE) fails++;      /* v1 passes the absolute checks */
                    strategy_v2_fixed(&f, &b);
                    if (hazard(&f, &b) != DIS_NONE) fails++;      /* so does v2.1 */
                }
    assert(fails == 0);

    /* agreement case: low battery, mid lap, moderate braking -> v2 only differs by rear shift */
    bbw_frame_t f = frame(300, 30, 0.5, 0.5e6);
    strategy_v1(&f, &a); strategy_v2(&f, &b);
    int c = classify(&f, &a, &b, 1, 50);
    printf("high speed, soc 0.50: v1 regen %.0f N, v2 regen %.0f N -> %s\n", a.f_regen, b.f_regen, dis_name(c));

    /* battery near full: v2 charges where v1 eases off */
    f = frame(250, 30, 0.86, 0.5e6);
    strategy_v1(&f, &a); strategy_v2(&f, &b);
    c = classify(&f, &a, &b, 1, 50);
    printf("soc 0.86: v1 %.0f kW, v2 %.0f kW -> %s\n", a.f_regen * f.v / 1e3, b.f_regen * f.v / 1e3, dis_name(c));
    assert(c == DIS_OVER_BATTERY);

    /* lap cap used up: v1 stops harvesting, v2 does not */
    f = frame(250, 30, 0.5, LAP_HARVEST_CAP_J + 1);
    strategy_v1(&f, &a); strategy_v2(&f, &b);
    c = classify(&f, &a, &b, 1, 50);
    printf("cap used: v1 regen %.0f N, v2 regen %.0f N -> %s\n", a.f_regen, b.f_regen, dis_name(c));
    assert(a.f_regen == 0);
    assert(c == DIS_OVER_LAP_CAP);

    /* slow corner, hard braking: v2's rear shift can exceed grip */
    f = frame(90, 50, 0.5, 0.5e6);
    strategy_v1(&f, &a); strategy_v2(&f, &b);
    c = classify(&f, &a, &b, 1, 50);
    printf("slow hard stop: grip %.0f N, v1 rear %.0f N, v2 rear %.0f N -> %s\n",
           f.f_rear_grip, a.f_regen + a.f_friction, b.f_regen + b.f_friction, dis_name(c));
    assert(c == DIS_REAR_LOCK_RISK);

    assert(classify(&f, &a, &b, 0, 50) == DIS_SHADOW_MISSING);

    /* absolute, not relative: a flaw v1 and v2 SHARE is still caught */
    assert(classify(&f, &b, &b, 1, 50) == DIS_REAR_LOCK_RISK);

    /* a single frame of a hard limit is hard; soft limits are not */
    assert(dis_is_hard(DIS_REAR_LOCK_RISK) && dis_is_hard(DIS_DEMAND_NOT_MET));
    assert(!dis_is_hard(DIS_OVER_BATTERY) && !dis_is_hard(DIS_OVER_MOTOR));

    /* nonsense output is treated as no answer, never as a decision */
    bbw_cmd_t nan_cmd = { .seq = f.seq, .f_regen = NAN, .f_friction = 0 };
    bbw_cmd_t neg_cmd = { .seq = f.seq, .f_regen = 0, .f_friction = -500 };
    assert(!cmd_plausible(&nan_cmd) && !cmd_plausible(&neg_cmd));
    assert(classify(&f, &a, &nan_cmd, 1, 50) == DIS_SHADOW_MISSING);

    /* v3: lap energy used up, motor not allowed to harvest, and the discs are
     * not told to make up the difference -> braking demand not met */
    f = frame(250, 30, 0.5, LAP_HARVEST_CAP_J + 1);
    strategy_v1(&f, &a); strategy_v3(&f, &b);
    c = classify(&f, &a, &b, 1, 50);
    printf("v3, cap used: asked %.0f N, v1 rear %.0f N, v3 rear %.0f N -> %s\n",
           f.f_rear_req, a.f_regen + a.f_friction, b.f_regen + b.f_friction, dis_name(c));
    assert(c == DIS_DEMAND_NOT_MET);
    /* ... but with charge allowed, v3 behaves */
    f = frame(250, 30, 0.5, 0.5e6);
    strategy_v1(&f, &a); strategy_v3(&f, &b);
    assert(hazard(&f, &b) == DIS_NONE);

    /* hot motor: v1 backs off, v2 ignores the temperature limit */
    f = frame(250, 30, 0.5, 0.5e6);
    f.motor_temp_c = 150;
    strategy_v1(&f, &a); strategy_v2(&f, &b);
    c = classify(&f, &a, &b, 1, 50);
    printf("motor 150C: limit %.0f N, v1 regen %.0f N, v2 regen %.0f N -> %s\n",
           car_motor_force_limit(f.v, 150, 1), a.f_regen, b.f_regen, dis_name(c));
    assert(a.f_regen <= car_motor_force_limit(f.v, 150, 1) + 1e-6);
    assert(c == DIS_OVER_MOTOR);

    /* metamorphic checks: when conditions get worse, v1 must get more careful, never less */
    int meta_fail = 0;
    for (double v = 60; v <= 340; v += 40)
        for (double acc = 10; acc <= 50; acc += 10) {
            bbw_frame_t lo = frame(v, acc, 0.5, 0.5e6), hi = lo;
            hi.f_rear_grip = lo.f_rear_grip * 0.8;               /* less grip */
            strategy_v1(&lo, &a); strategy_v1(&hi, &b);
            if (b.f_regen + b.f_friction > a.f_regen + a.f_friction + 1e-6) meta_fail++;
            hi = lo; hi.motor_temp_c = 150;                      /* hotter motor */
            strategy_v1(&lo, &a); strategy_v1(&hi, &b);
            if (b.f_regen > a.f_regen + 1e-6) meta_fail++;
            if (fabs((b.f_regen + b.f_friction) - (a.f_regen + a.f_friction)) > 1e-6) meta_fail++;  /* discs make up */
            hi = lo; hi.soc = 0.95;                              /* fuller battery */
            strategy_v1(&lo, &a); strategy_v1(&hi, &b);
            if (b.f_regen > a.f_regen + 1e-6) meta_fail++;
        }
    assert(meta_fail == 0);

    /* monitor self-test: every check must fire on a command built to break it */
    f = frame(250, 30, 0.5, 0.5e6);
    bbw_cmd_t bad;
    bad = (bbw_cmd_t){ .seq = f.seq, .f_regen = 0, .f_friction = f.f_rear_grip * 1.2 };
    assert(hazard(&f, &bad) == DIS_REAR_LOCK_RISK);
    bad = (bbw_cmd_t){ .seq = f.seq, .f_regen = 0, .f_friction = f.f_rear_req * 0.5 };
    assert(hazard(&f, &bad) == DIS_DEMAND_NOT_MET);
    bbw_frame_t fc = frame(250, 30, 0.5, LAP_HARVEST_CAP_J + 1);
    bad = (bbw_cmd_t){ .seq = fc.seq, .f_regen = 2000, .f_friction = fc.f_rear_req - 2000 };
    assert(hazard(&fc, &bad) == DIS_OVER_LAP_CAP);
    bbw_frame_t fb = frame(250, 30, 0.97, 0.5e6);
    bad = (bbw_cmd_t){ .seq = fb.seq, .f_regen = 3000, .f_friction = fb.f_rear_req - 3000 };
    assert(hazard(&fb, &bad) == DIS_OVER_BATTERY);
    bbw_frame_t fm = frame(250, 30, 0.5, 0.5e6); fm.motor_temp_c = 155;
    bad = (bbw_cmd_t){ .seq = fm.seq, .f_regen = 3000, .f_friction = fm.f_rear_req - 3000 };
    assert(hazard(&fm, &bad) == DIS_OVER_MOTOR);
    printf("metamorphic checks and monitor self-tests: ok\n");

    /* wings still open at the start of braking: less grip than with them closed */
    assert(car_rear_grip_aero(80, 0.0, 1.0) < car_rear_grip_aero(80, 1.0, 1.0));
    assert(fabs(car_rear_grip_aero(80, 1.0, 1.0) - car_rear_grip(80)) < 1e-6);
    printf("test_control: ok\n");
    return 0;
}
