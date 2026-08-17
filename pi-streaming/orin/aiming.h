/**
 * aiming.h
 *
 * Geometry for the Pi-side lock-on: turn a detection bounding box plus the
 * capture-time servo pose into an absolute pan/tilt command, plus a range
 * estimate. The Orin-receiver callback calls into here.
 *
 * Pipeline position:
 *   detection (bbox, frame_id) --[receiver]--> join frame_id -> capture pose
 *   --[here]--> absolute servo command --> ddl_servo_set_target + slew event.
 *
 * Model:
 *   Rectilinear pinhole. A target at horizontal pixel offset from centre maps
 *   to a ray angle via  theta = atan( (offset / half_width) * tan(HFOV/2) ).
 *   Because the box was captured while the camera was AT capture_pose, the new
 *   command is simply  capture_pose + theta  (with a mount-dependent sign).
 *
 * IMPORTANT — two values that must match the rest of the system:
 *   1. (frame_w, frame_h) is the pixel coordinate space the *bbox* lives in.
 *      The Orin MUST return boxes in this same space: full 1920x1080
 *      source-frame pixels, with the detector's letterbox already reversed.
 *      This is a hard contract, not a preference - aim_compute() and the
 *      app-facing JSON in ddl_bridge.c both assume it.
 *   2. (hfov_deg, vfov_deg) must be the FOV of THAT frame — i.e. the effective
 *      FOV after the sensor crop the 1080p path uses. The defaults were
 *      measured on this rig with script/probe_camera_fov.py; see aiming.c for
 *      the measurement and when to re-run it.
 */

#ifndef ORIN_AIMING_H
#define ORIN_AIMING_H

#include <stdbool.h>

typedef struct
{
    /* Optics — FOV of the bbox reference frame, degrees. See header note 2. */
    float hfov_deg;
    float vfov_deg;

    /* Pixel coordinate space the bbox is expressed in. See header note 1. */
    int   frame_w;
    int   frame_h;

    /* Mount-dependent direction mapping (+1 or -1):
     *   pan_sign:  multiplies the image-right angular offset to get the servo
     *              pan direction. Flip if locking sends the camera the wrong
     *              way horizontally.
     *   tilt_sign: same for image-down -> servo tilt. */
    float pan_sign;
    float tilt_sign;

    /* Servo travel limits (degrees) for clamping the command. */
    float pan_min_deg,  pan_max_deg;
    float tilt_min_deg, tilt_max_deg;
} AimConfig;

typedef struct
{
    float pan_deg;          /* Absolute servo pan command, clamped to limits.  */
    float tilt_deg;         /* Absolute servo tilt command, clamped to limits. */
    float pan_offset_deg;   /* Signed angular offset applied (pre-clamp).       */
    float tilt_offset_deg;
    bool  clamped;          /* True if either axis was clamped to a limit.      */
} AimSolution;

/**
 * aim_config_default - Fill cfg with the project defaults.
 *
 * FOV defaults are MEASURED on this rig (IMX477 + 16 mm; see aiming.c for the
 * date and the arithmetic). Frame defaults to 1920x1080. Servo limits come from
 * the SERVO_*_{MIN,MAX}_ANGLE_DEG values. Both signs are verified against the
 * rig; re-verify with a manual jog whenever a servo axis is remounted.
 */
void aim_config_default(AimConfig *cfg);

/**
 * aim_compute - Bounding box + capture pose -> absolute servo command.
 *
 * @cfg:              Optics / frame / limits configuration.
 * @bbox_x,_y,_w,_h:  Box in the cfg->frame_w x cfg->frame_h pixel space
 *                    (top-left origin, +x right, +y down). Width/height > 0.
 * @capture_pan_deg:  Servo pan at the instant the frame was captured.
 * @capture_tilt_deg: Servo tilt at capture.
 * @out:              Filled on success.
 *
 * Returns false on bad arguments (NULL, non-positive frame/box dims), true
 * otherwise. A clamped solution still returns true with out->clamped set.
 */
bool aim_compute(const AimConfig *cfg,
                 int bbox_x, int bbox_y, int bbox_w, int bbox_h,
                 float capture_pan_deg, float capture_tilt_deg,
                 AimSolution *out);

/**
 * aim_estimate_distance_m - Range from box height and known target height.
 *
 * Uses the same rectilinear model:
 *   D = (target_height_m * frame_h) / (2 * bbox_h * tan(VFOV/2))
 * Distance is NOT a detector output; it is computed here from geometry, so it
 * is only as good as target_height_m and the VFOV calibration.
 *
 * @cfg:                Configuration (uses vfov_deg and frame_h).
 * @bbox_h:             Box height in pixels (> 0).
 * @target_height_m:    Known real-world target height in metres (> 0).
 *
 * Returns the distance in metres, or -1.0f on bad arguments.
 */
float aim_estimate_distance_m(const AimConfig *cfg, int bbox_h,
                              float target_height_m);

#endif /* ORIN_AIMING_H */
