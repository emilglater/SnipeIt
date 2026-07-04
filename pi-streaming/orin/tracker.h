/**
 * tracker.h
 *
 * Pi-side multi-target tracker: turns the Orin's per-frame detection indices
 * into STABLE track ids that persist across frames, so the operator can lock
 * onto a specific target and follow it, and the app shows consistent ids.
 *
 * Why it lives on the Pi (not the Orin): the camera pans/tilts constantly (fast
 * during the serpentine scan), so a world-stationary target sweeps across the
 * image frame-to-frame. Associating in IMAGE space would break. Instead we
 * MOTION-COMPENSATE using the frame_id->pose ring: each detection is mapped to
 * its absolute world bearing (abs_pan, abs_tilt) via the same aiming geometry,
 * and association happens in that world-stable angular space. Pose lives on the
 * Pi, so the tracker does too.
 *
 * Algorithm: greedy nearest-neighbour association in angular space with a
 * per-track lifecycle (spawn / coast / delete) and EMA bearing smoothing.
 * Deterministic and light — sufficient for the small target counts here. See
 * TRACKER_DESIGN.md. A constant-velocity bearing predictor is a documented
 * extension point (tracker.c) if fast slews ever move a target more than the
 * gate between frames.
 *
 * Threading: all state is touched only on the Orin receiver thread (inside
 * orin_detection_handler), so the tracker needs no lock of its own.
 */

#ifndef ORIN_TRACKER_H
#define ORIN_TRACKER_H

#include <stdbool.h>
#include <stdint.h>

#include "aiming.h"
#include "detection_msg.h"
#include "pose_ring.h"

#define TRACKER_MAX_TRACKS 32

typedef struct Tracker Tracker;

/**
 * tracker_create - Allocate a tracker.
 * @aim: aiming config used for the bbox->bearing geometry. BORROWED (not owned);
 *       must outlive the tracker. Reads FOV/frame/sign; never mutated.
 * Returns the tracker, or NULL on bad arg / alloc failure.
 */
Tracker *tracker_create(const AimConfig *aim);

/** tracker_destroy - Free the tracker. NULL-safe. */
void tracker_destroy(Tracker *t);

/**
 * tracker_update - Associate one frame's detections and assign stable ids.
 *
 * Call once per detection message, ONLY when the frame_id->pose join HIT (the
 * pose is required for the angular mapping).
 *
 * @t:             The tracker.
 * @msg:           The parsed detection message for this frame.
 * @pose:          The capture-time pose joined for msg->frame_id (non-NULL).
 * @out_ids:       Filled with the stable track id for detections[i]
 *                 (caller array, length >= msg->num_detections). Never 0 on
 *                 return for a valid detection.
 * @out_confirmed: Filled with whether detections[i]'s track is confirmed
 *                 (hits >= N_INIT) — safe to allow locking. Same length.
 */
void tracker_update(Tracker *t, const OrinDetectionMsg *msg, const PoseEntry *pose,
                    uint32_t *out_ids, bool *out_confirmed);

/** tracker_active_count - Number of live tracks (diagnostic). */
int tracker_active_count(const Tracker *t);

#endif /* ORIN_TRACKER_H */
