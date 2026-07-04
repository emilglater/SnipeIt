/**
 * test_tracker.c — unit tests for the angular-space tracker.
 *
 * Build/run:  make test_tracker && ./test_tracker
 *
 * The key property under test is MOTION COMPENSATION: a world-stationary target
 * must keep ONE id while the camera pans (its image box moves, its bearing does
 * not). We build each detection's bbox by INVERTING aim_compute for a desired
 * world bearing at a given pose, so a "still" target at a fixed bearing yields a
 * different bbox as the pose changes — exactly the real situation.
 */

#include "tracker.h"
#include "aiming.h"
#include "detection_msg.h"
#include "pose_ring.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); g_fail++; } \
} while (0)

/* Inverse of aim_compute: pixel bbox (top-left) whose CENTRE maps to the desired
 * absolute world bearing at the given capture pose. */
static void box_for_bearing(const AimConfig *c, float pose_pan, float pose_tilt,
                            float want_pan, float want_tilt, int w, int h,
                            int *x, int *y)
{
    float theta_x = (want_pan  - pose_pan)  / c->pan_sign;   /* degrees */
    float theta_y = (want_tilt - pose_tilt) / c->tilt_sign;
    float ndx = tanf(theta_x * (float)M_PI / 180.0f) /
                tanf(c->hfov_deg * 0.5f * (float)M_PI / 180.0f);
    float ndy = tanf(theta_y * (float)M_PI / 180.0f) /
                tanf(c->vfov_deg * 0.5f * (float)M_PI / 180.0f);
    float half_w = c->frame_w * 0.5f, half_h = c->frame_h * 0.5f;
    *x = (int)lroundf(half_w + ndx * half_w - w * 0.5f);
    *y = (int)lroundf(half_h + ndy * half_h - h * 0.5f);
}

static PoseEntry pose_at(float pan, float tilt, uint64_t ts)
{
    PoseEntry p = { .frame_id = 0, .hor_angle = pan, .ver_angle = tilt,
                    .capture_ts_ms = ts };
    return p;
}

static void set_det(OrinDetection *d, const char *cls, int x, int y, int w, int h)
{
    memset(d, 0, sizeof(*d));
    strncpy(d->cls, cls, sizeof(d->cls) - 1);
    strncpy(d->target_id, "1", sizeof(d->target_id) - 1);
    d->confidence = 0.9f;
    d->bbox_x = x; d->bbox_y = y; d->bbox_w = w; d->bbox_h = h;
}

/* --- Test A: motion comp — one still target keeps its id as the camera pans. */
static void test_motion_comp_stable_id(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    Tracker *t = tracker_create(&cfg);
    CHECK(t != NULL, "create");

    uint32_t ids[ORIN_MAX_DETECTIONS]; bool conf[ORIN_MAX_DETECTIONS];
    int x, y;

    /* Frame 1: camera at pan 90, target at world bearing (95, 88). */
    OrinDetectionMsg m1; memset(&m1, 0, sizeof(m1));
    m1.num_detections = 1;
    box_for_bearing(&cfg, 90, 90, 95, 88, 200, 400, &x, &y);
    set_det(&m1.detections[0], "HUMAN", x, y, 200, 400);
    PoseEntry p1 = pose_at(90, 90, 1000);
    tracker_update(t, &m1, &p1, ids, conf);
    uint32_t id1 = ids[0];
    CHECK(id1 != 0, "A: got an id");
    CHECK(conf[0] == false, "A: not confirmed after 1 hit");

    /* Frame 2: camera PANNED to 93 (box moves in image), SAME world target. */
    OrinDetectionMsg m2; memset(&m2, 0, sizeof(m2));
    m2.num_detections = 1;
    int x2, y2;
    box_for_bearing(&cfg, 93, 91, 95, 88, 210, 410, &x2, &y2);
    set_det(&m2.detections[0], "HUMAN", x2, y2, 210, 410);
    PoseEntry p2 = pose_at(93, 91, 1150);
    tracker_update(t, &m2, &p2, ids, conf);
    CHECK(x2 != x || y2 != y, "A: box actually moved in the image");
    CHECK(ids[0] == id1, "A: same id despite camera pan (motion compensated)");
    CHECK(conf[0] == true, "A: confirmed after 2 hits (N_INIT)");

    tracker_destroy(t);
}

/* --- Test B: two separated targets get and keep distinct ids. */
static void test_two_targets_distinct(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    Tracker *t = tracker_create(&cfg);
    uint32_t ids[ORIN_MAX_DETECTIONS]; bool conf[ORIN_MAX_DETECTIONS];
    int xa, ya, xb, yb;

    OrinDetectionMsg m; memset(&m, 0, sizeof(m));
    m.num_detections = 2;
    box_for_bearing(&cfg, 90, 90, 92, 90, 200, 400, &xa, &ya);   /* bearing 92 */
    box_for_bearing(&cfg, 90, 90, 98, 90, 200, 400, &xb, &yb);   /* bearing 98 (6 deg apart) */
    set_det(&m.detections[0], "HUMAN", xa, ya, 200, 400);
    set_det(&m.detections[1], "HUMAN", xb, yb, 200, 400);
    PoseEntry p = pose_at(90, 90, 1000);
    tracker_update(t, &m, &p, ids, conf);
    CHECK(ids[0] != 0 && ids[1] != 0, "B: both ids assigned");
    CHECK(ids[0] != ids[1], "B: two targets -> distinct ids");
    uint32_t a = ids[0], b = ids[1];

    /* Next frame, same two targets, camera nudged: ids stable. */
    PoseEntry p2 = pose_at(91, 90, 1150);
    box_for_bearing(&cfg, 91, 90, 92, 90, 205, 405, &xa, &ya);
    box_for_bearing(&cfg, 91, 90, 98, 90, 205, 405, &xb, &yb);
    set_det(&m.detections[0], "HUMAN", xa, ya, 205, 405);
    set_det(&m.detections[1], "HUMAN", xb, yb, 205, 405);
    tracker_update(t, &m, &p2, ids, conf);
    CHECK(ids[0] == a && ids[1] == b, "B: ids stable across frames");
    CHECK(tracker_active_count(t) == 2, "B: exactly 2 live tracks");
    tracker_destroy(t);
}

/* --- Test C: a target that ages past MAX_COAST_MS is dropped; reappearance is
 *     a NEW id. */
static void test_coast_drop_reappear(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    Tracker *t = tracker_create(&cfg);
    uint32_t ids[ORIN_MAX_DETECTIONS]; bool conf[ORIN_MAX_DETECTIONS];
    int x, y;

    OrinDetectionMsg m; memset(&m, 0, sizeof(m));
    m.num_detections = 1;
    box_for_bearing(&cfg, 90, 90, 95, 90, 200, 400, &x, &y);
    set_det(&m.detections[0], "HUMAN", x, y, 200, 400);
    PoseEntry p = pose_at(90, 90, 1000);
    tracker_update(t, &m, &p, ids, conf);
    uint32_t id1 = ids[0];

    /* An empty frame ~1.7 s later ages the track out (> MAX_COAST_MS=1500). */
    OrinDetectionMsg empty; memset(&empty, 0, sizeof(empty));
    empty.num_detections = 0;
    PoseEntry p_late = pose_at(90, 90, 2800);
    tracker_update(t, &empty, &p_late, ids, conf);
    CHECK(tracker_active_count(t) == 0, "C: stale track dropped");

    /* Same bearing reappears -> new id. */
    PoseEntry p3 = pose_at(90, 90, 3000);
    tracker_update(t, &m, &p3, ids, conf);
    CHECK(ids[0] != 0 && ids[0] != id1, "C: reappearance gets a NEW id");
    tracker_destroy(t);
}

/* --- Test D: a target jumping far beyond the gate is NOT associated (new id). */
static void test_teleport_beyond_gate(void)
{
    AimConfig cfg; aim_config_default(&cfg);
    Tracker *t = tracker_create(&cfg);
    uint32_t ids[ORIN_MAX_DETECTIONS]; bool conf[ORIN_MAX_DETECTIONS];
    int x, y;

    OrinDetectionMsg m; memset(&m, 0, sizeof(m));
    m.num_detections = 1;
    box_for_bearing(&cfg, 90, 90, 93, 90, 200, 400, &x, &y);
    set_det(&m.detections[0], "HUMAN", x, y, 200, 400);
    PoseEntry p = pose_at(90, 90, 1000);
    tracker_update(t, &m, &p, ids, conf);
    uint32_t id1 = ids[0];

    /* Next frame, same pose, target at bearing 100 (7 deg jump >> gate). */
    box_for_bearing(&cfg, 90, 90, 100, 90, 200, 400, &x, &y);
    set_det(&m.detections[0], "HUMAN", x, y, 200, 400);
    PoseEntry p2 = pose_at(90, 90, 1150);
    tracker_update(t, &m, &p2, ids, conf);
    CHECK(ids[0] != id1, "D: far jump -> not associated, new id");
    tracker_destroy(t);
}

int main(void)
{
    test_motion_comp_stable_id();
    test_two_targets_distinct();
    test_coast_drop_reappear();
    test_teleport_beyond_gate();

    if (g_fail == 0) { printf("PASS: all tracker tests\n"); return 0; }
    printf("FAILED: %d check(s)\n", g_fail);
    return 1;
}
