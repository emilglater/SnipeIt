/**
 * aiming.c
 *
 * Implementation of the bbox + pose -> servo command geometry.
 * See aiming.h for the model and the two values that must match the system.
 */

#include "aiming.h"

#include <math.h>
#include <stddef.h>

/* Servo travel limits and sign defaults come from the DDL servo config so the
 * geometry clamps to the same range the servo FSM accepts. */
#include "ddl/servo/servo_config.h"

/* MEASURED FOV for the 1080p capture path (probe_camera_fov.py, 2026-07-01,
 * IMX477 + 16mm on this rig). picamera2's config for main=1920x1080 selects the
 * 2028x1080 sensor mode and the ISP applies ScalerCrop (108, 440, 3840, 2160) —
 * i.e. a 3840x2160 sensor rectangle (1.55um pitch) mapped to the 1080p output:
 *
 *   HFOV = 2*atan( 3840*1.55um / (2*16mm) )  = 21.07 deg
 *   VFOV = 2*atan( 2160*1.55um / (2*16mm) )  = 11.95 deg
 *
 * Re-run probe_camera_fov.py and update these if the lens, sensor mode, or the
 * main-stream size/aspect changes.
 *
 * CAVEAT: measured under the old picamera2 capture path; the pipeline now uses
 * libcamerasrc, which negotiates its own sensor mode and ScalerCrop. A live run
 * on 2026-08-16 confirmed libcamerasrc selects the SAME 2028x1080 sensor mode
 * for a 1920x1080 output, so the mode matches. The ScalerCrop it applies has
 * not been checked; confirm that before trusting these to sub-degree accuracy. */
#define AIM_DEFAULT_HFOV_DEG   21.07f
#define AIM_DEFAULT_VFOV_DEG   11.95f
#define AIM_DEFAULT_FRAME_W    1920
#define AIM_DEFAULT_FRAME_H    1080

static float deg2rad(float d) { return d * (float)(M_PI / 180.0); }
static float rad2deg(float r) { return r * (float)(180.0 / M_PI); }

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void aim_config_default(AimConfig *cfg)
{
    if (cfg == NULL)
    {
        return;
    }
    cfg->hfov_deg     = AIM_DEFAULT_HFOV_DEG;
    cfg->vfov_deg     = AIM_DEFAULT_VFOV_DEG;
    cfg->frame_w      = AIM_DEFAULT_FRAME_W;
    cfg->frame_h      = AIM_DEFAULT_FRAME_H;
    /* Signs VERIFIED on the rig: pan on 2026-07-01 (commanding pan 90->130
     * turned the camera LEFT, so a right-of-image target must LOWER the pan
     * angle -> pan_sign=-1). Tilt RE-verified 2026-07-06 after the vertical
     * mount changed (range now 70..150, level ~110): increasing the tilt
     * angle now pitches the camera DOWN, so a bottom-of-image target must
     * RAISE the tilt angle -> tilt_sign=+1. Re-verify both signs with a
     * manual jog whenever a servo axis is remounted. */
    cfg->pan_sign     = -1.0f;
    cfg->tilt_sign    = +1.0f;
    cfg->pan_min_deg  = SERVO_HORIZONTAL_MIN_ANGLE_DEG;
    cfg->pan_max_deg  = SERVO_HORIZONTAL_MAX_ANGLE_DEG;
    cfg->tilt_min_deg = SERVO_VERTICAL_MIN_ANGLE_DEG;
    cfg->tilt_max_deg = SERVO_VERTICAL_MAX_ANGLE_DEG;
}

/* Map a normalised image offset (-1..+1 across the half-extent) to a ray angle
 * in degrees using the rectilinear model. */
static float offset_to_angle_deg(float normalised_offset, float fov_deg)
{
    return rad2deg(atanf(normalised_offset * tanf(deg2rad(fov_deg) * 0.5f)));
}

bool aim_compute(const AimConfig *cfg,
                 int bbox_x, int bbox_y, int bbox_w, int bbox_h,
                 float capture_pan_deg, float capture_tilt_deg,
                 AimSolution *out)
{
    if (cfg == NULL || out == NULL)
    {
        return false;
    }
    if (cfg->frame_w <= 0 || cfg->frame_h <= 0 || bbox_w <= 0 || bbox_h <= 0)
    {
        return false;
    }

    /* Box centre in pixels. */
    float cx = (float)bbox_x + (float)bbox_w * 0.5f;
    float cy = (float)bbox_y + (float)bbox_h * 0.5f;

    /* Normalise to -1..+1 about the frame centre (clamped: a box centre can sit
     * slightly outside the frame after the Orin's letterbox reversal). */
    float half_w = (float)cfg->frame_w * 0.5f;
    float half_h = (float)cfg->frame_h * 0.5f;
    float ndx = clampf((cx - half_w) / half_w, -1.0f, 1.0f);
    float ndy = clampf((cy - half_h) / half_h, -1.0f, 1.0f);

    float theta_x = offset_to_angle_deg(ndx, cfg->hfov_deg);
    float theta_y = offset_to_angle_deg(ndy, cfg->vfov_deg);

    out->pan_offset_deg  = cfg->pan_sign  * theta_x;
    out->tilt_offset_deg = cfg->tilt_sign * theta_y;

    float pan_raw  = capture_pan_deg  + out->pan_offset_deg;
    float tilt_raw = capture_tilt_deg + out->tilt_offset_deg;

    out->pan_deg  = clampf(pan_raw,  cfg->pan_min_deg,  cfg->pan_max_deg);
    out->tilt_deg = clampf(tilt_raw, cfg->tilt_min_deg, cfg->tilt_max_deg);
    out->clamped  = (out->pan_deg != pan_raw) || (out->tilt_deg != tilt_raw);

    return true;
}

float aim_estimate_distance_m(const AimConfig *cfg, int bbox_h,
                              float target_height_m)
{
    if (cfg == NULL || bbox_h <= 0 || target_height_m <= 0.0f ||
        cfg->frame_h <= 0)
    {
        return -1.0f;
    }

    /* D = (target_height * frame_h) / (2 * bbox_h * tan(VFOV/2)) */
    float denom = 2.0f * (float)bbox_h * tanf(deg2rad(cfg->vfov_deg) * 0.5f);
    if (denom <= 0.0f)
    {
        return -1.0f;
    }
    return (target_height_m * (float)cfg->frame_h) / denom;
}
