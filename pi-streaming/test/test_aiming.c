/**
 * test_aiming.c
 *
 * Unit test for the bbox + pose -> servo command geometry.
 * Self-contained (needs libm + the servo_config.h macros via -I../src).
 * Build/run:  make test_aiming && ./test_aiming
 */

#include "aiming.h"

#include <math.h>
#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg,             \
                    __FILE__, __LINE__);                           \
            g_failures++;                                           \
        }                                                          \
    } while (0)

#define CHECK_NEAR(a, b, tol, msg)                                  \
    do {                                                            \
        double _d = fabs((double)(a) - (double)(b));               \
        if (_d > (tol)) {                                           \
            fprintf(stderr, "FAIL: %s (got %.5f want %.5f, |d|=%.5f) (%s:%d)\n", \
                    msg, (double)(a), (double)(b), _d, __FILE__, __LINE__); \
            g_failures++;                                           \
        }                                                          \
    } while (0)

/* A box whose centre lands on (cx, cy) of size w x h. */
static void box_at(int cx, int cy, int w, int h, int *x, int *y)
{
    *x = cx - w / 2;
    *y = cy - h / 2;
}

static void test_centered_is_no_op(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    int x, y; box_at(cfg.frame_w / 2, cfg.frame_h / 2, 100, 200, &x, &y);

    AimSolution s;
    CHECK(aim_compute(&cfg, x, y, 100, 200, 90.0f, 90.0f, &s), "centered computes");
    CHECK_NEAR(s.pan_offset_deg, 0.0f, 1e-4, "centered pan offset ~0");
    CHECK_NEAR(s.tilt_offset_deg, 0.0f, 1e-4, "centered tilt offset ~0");
    CHECK_NEAR(s.pan_deg, 90.0f, 1e-4, "centered pan == capture");
    CHECK_NEAR(s.tilt_deg, 90.0f, 1e-4, "centered tilt == capture");
    CHECK(!s.clamped, "centered not clamped");
}

static void test_right_edge_is_half_hfov(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    /* Centre on the right edge -> normalised +1 -> theta = HFOV/2. */
    int x, y; box_at(cfg.frame_w, cfg.frame_h / 2, 40, 40, &x, &y);

    AimSolution s;
    CHECK(aim_compute(&cfg, x, y, 40, 40, 90.0f, 90.0f, &s), "right-edge computes");
    CHECK_NEAR(s.pan_offset_deg, cfg.pan_sign * cfg.hfov_deg / 2.0f, 1e-3,
               "right edge -> +HFOV/2");
    CHECK_NEAR(s.pan_deg, 90.0f + cfg.pan_sign * cfg.hfov_deg / 2.0f, 1e-3,
               "right-edge pan command");
}

static void test_left_edge_is_negative_half_hfov(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    int x, y; box_at(0, cfg.frame_h / 2, 40, 40, &x, &y);

    AimSolution s;
    CHECK(aim_compute(&cfg, x, y, 40, 40, 90.0f, 90.0f, &s), "left-edge computes");
    CHECK_NEAR(s.pan_offset_deg, -cfg.pan_sign * cfg.hfov_deg / 2.0f, 1e-3,
               "left edge -> -HFOV/2");
}

static void test_bottom_edge_uses_tilt_sign(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    int x, y; box_at(cfg.frame_w / 2, cfg.frame_h, 40, 40, &x, &y);

    AimSolution s;
    CHECK(aim_compute(&cfg, x, y, 40, 40, 90.0f, 90.0f, &s), "bottom-edge computes");
    /* normalised +1 vertical -> theta = VFOV/2, then * tilt_sign. */
    CHECK_NEAR(s.tilt_offset_deg, cfg.tilt_sign * cfg.vfov_deg / 2.0f, 1e-3,
               "bottom edge -> tilt_sign * VFOV/2");
}

static void test_clamping(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    int x, y; box_at(cfg.frame_w, cfg.frame_h / 2, 40, 40, &x, &y);  /* full right */

    /* pan_sign=-1 (rig-verified): a right-of-image target LOWERS the pan angle.
     * From a capture pose near the pan min, that overshoots the min and clamps. */
    AimSolution s;
    CHECK(aim_compute(&cfg, x, y, 40, 40, cfg.pan_min_deg + 2.0f, 90.0f, &s),
          "near-limit computes");
    CHECK(s.clamped, "overshoot is clamped");
    CHECK_NEAR(s.pan_deg, cfg.pan_min_deg, 1e-4, "pan clamped to min");
}

static void test_distance(void)
{
    AimConfig cfg; aim_config_default(&cfg);

    /* Known target 0.30 m tall, box 108 px in a 1080 px frame. */
    float d = aim_estimate_distance_m(&cfg, 108, 0.30f);
    /* D = 0.30 * 1080 / (2 * 108 * tan(VFOV/2)) */
    float expect = 0.30f * cfg.frame_h /
                   (2.0f * 108.0f * tanf((cfg.vfov_deg * (float)(M_PI / 180.0)) * 0.5f));
    CHECK_NEAR(d, expect, 1e-3, "distance matches closed form");
    CHECK(d > 0.0f, "distance positive");

    /* A box twice as tall must read half the distance. */
    float d2 = aim_estimate_distance_m(&cfg, 216, 0.30f);
    CHECK_NEAR(d2, d / 2.0f, 1e-3, "twice the pixels -> half the range");

    CHECK(aim_estimate_distance_m(&cfg, 0, 0.30f) < 0.0f, "zero bbox_h rejected");
    CHECK(aim_estimate_distance_m(&cfg, 100, 0.0f) < 0.0f, "zero height rejected");
}

static void test_bad_args(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    AimSolution s;
    CHECK(!aim_compute(NULL, 0, 0, 10, 10, 90, 90, &s), "NULL cfg rejected");
    CHECK(!aim_compute(&cfg, 0, 0, 0, 10, 90, 90, &s), "zero width rejected");
    CHECK(!aim_compute(&cfg, 0, 0, 10, 0, 90, 90, &s), "zero height rejected");
    CHECK(!aim_compute(&cfg, 0, 0, 10, 10, 90, 90, NULL), "NULL out rejected");
}

int main(void)
{
    test_centered_is_no_op();
    test_right_edge_is_half_hfov();
    test_left_edge_is_negative_half_hfov();
    test_bottom_edge_uses_tilt_sign();
    test_clamping();
    test_distance();
    test_bad_args();

    if (g_failures == 0)
    {
        printf("PASS: all aiming tests\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
}
