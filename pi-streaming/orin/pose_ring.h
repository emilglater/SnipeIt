/**
 * pose_ring.h
 *
 * Thread-safe frame_id -> capture-time pose ring buffer for the Pi<->Orin
 * detection protocol.
 *
 * Design rationale:
 *   Every frame the Pi sends to the Orin is tagged with a frame_id. At the
 *   instant of capture the Pi records the servo pan/tilt pose for that
 *   frame_id here. Detections come back from the Orin echoing only the
 *   frame_id (the Orin is stateless about pose); the Pi joins frame_id ->
 *   pose locally from this buffer and feeds it to the geometry/aiming code.
 *
 *   Two threads touch this structure:
 *     - the sender records a pose at capture time (one writer)
 *     - the ZeroMQ receiver looks up a pose when a detection arrives (one reader)
 *   A single mutex guards both; the critical sections are O(capacity) with a
 *   tiny capacity (~16), so contention is negligible.
 *
 *   Capacity must cover the worst-case round trip
 *   (capture -> encode -> GigE transfer -> Orin inference -> return). Size it
 *   in TIME, not frames: at the ~2.4-3.4 fps the software encode sustains, 16
 *   entries is ~5-7 s of history, far more than the round trip needs. Older
 *   entries are silently overwritten; a lookup for an overwritten frame_id
 *   simply misses, which the caller treats as "too stale to aim on". A hit is
 *   necessary but not sufficient: ddl_bridge also gates frames captured inside
 *   the slew-settling window.
 */

#ifndef ORIN_POSE_RING_H
#define ORIN_POSE_RING_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

/* Default depth. 16 covers the capture->inference->return round trip with
 * generous headroom at any frame rate this pipeline has run at. */
#define POSE_RING_DEFAULT_CAPACITY 16

/* One recorded frame: the servo pose at the instant the frame was captured. */
typedef struct
{
    uint32_t frame_id;        /* Tag carried out to the Orin and echoed back.    */
    float    hor_angle;       /* Servo pan angle at capture, degrees.            */
    float    ver_angle;       /* Servo tilt angle at capture, degrees.           */
    uint64_t capture_ts_ms;   /* CLOCK_MONOTONIC ms at capture, for staleness.   */
} PoseEntry;

typedef struct
{
    PoseEntry      *entries;      /* Circular array, length == capacity.          */
    int             capacity;     /* Number of frames retained.                   */
    int             write_pos;    /* Next slot to overwrite (0..capacity-1).      */
    uint64_t        count;        /* Total recorded since creation (monotonic).   */
    pthread_mutex_t mutex;        /* Guards entries/write_pos/count.              */
} PoseRing;

/**
 * pose_ring_create - Allocate and initialise a pose ring buffer.
 *
 * @capacity: Number of frame->pose entries to retain. Pass
 *            POSE_RING_DEFAULT_CAPACITY unless you have a reason not to.
 *
 * Returns a pointer to the new ring, or NULL on bad argument / allocation
 * failure. The caller must eventually call pose_ring_destroy().
 */
PoseRing *pose_ring_create(int capacity);

/**
 * pose_ring_destroy - Free all memory associated with a pose ring.
 *
 * @r: Ring to destroy. May be NULL (no-op).
 */
void pose_ring_destroy(PoseRing *r);

/**
 * pose_ring_record - Record the capture-time pose for a frame_id.
 *
 * Called by the sender once per outgoing frame, at the instant of capture.
 * Overwrites the oldest entry when the ring is full.
 *
 * @r:             The ring.
 * @frame_id:      The id tagged onto the outgoing frame.
 * @hor_angle:     Servo pan angle at capture, degrees.
 * @ver_angle:     Servo tilt angle at capture, degrees.
 * @capture_ts_ms: CLOCK_MONOTONIC milliseconds at capture.
 *
 * Thread-safe: acquires the internal mutex.
 */
void pose_ring_record(PoseRing *r, uint32_t frame_id,
                      float hor_angle, float ver_angle,
                      uint64_t capture_ts_ms);

/**
 * pose_ring_lookup - Join a returned frame_id back to its capture-time pose.
 *
 * Called by the detection receiver when a message arrives from the Orin.
 *
 * @r:        The ring.
 * @frame_id: The frame_id echoed back in the detection message.
 * @out:      Filled with the matching entry on success. Untouched on miss.
 *
 * Returns true if the frame_id was still in the buffer, false if it has been
 * overwritten (detection arrived too late) or was never recorded. A false
 * return is normal and means "no pose available -> do not aim on this one".
 *
 * Thread-safe: acquires the internal mutex.
 */
bool pose_ring_lookup(PoseRing *r, uint32_t frame_id, PoseEntry *out);

/**
 * pose_ring_count - Total entries recorded since creation (monotonic).
 *
 * Useful for diagnostics / tests. Thread-safe: reads under mutex.
 */
uint64_t pose_ring_count(PoseRing *r);

#endif /* ORIN_POSE_RING_H */
