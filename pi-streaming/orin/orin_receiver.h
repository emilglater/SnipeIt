/**
 * orin_receiver.h
 *
 * ZeroMQ PULL receiver for detections coming back from the Orin.
 *
 * Topology (per the Pi<->Orin protocol): the Pi is the hub. Detections cross
 * the direct GigE link once, into the Pi. The Pi is the PULL/bind side and the
 * Orin is the PUSH/connect side, so the Orin can be (re)started freely without
 * the Pi needing to know its address.
 *
 * Lifecycle:
 *   - orin_receiver_start() binds the socket and spawns one receive thread.
 *   - For each message the thread: parses the JSON (detection_msg), joins
 *     frame_id -> capture-time pose via the supplied pose ring, then invokes
 *     the handler with the detection message and the matched pose.
 *   - orin_receiver_stop() signals the thread, joins it, and tears down zmq.
 *
 * THREADING CONTRACT: the handler runs on the receiver thread, NOT the main
 * loop. If it drives the servos it must use the thread-safe entry points
 * (e.g. ddl_servo_set_target, which takes the servo target mutex) exactly as
 * the WebSocket command path does.
 */

#ifndef ORIN_RECEIVER_H
#define ORIN_RECEIVER_H

#include "detection_msg.h"
#include "pose_ring.h"

typedef struct OrinReceiver OrinReceiver;

/**
 * Handler invoked once per received detection message.
 *
 * @msg:  The parsed detection message (valid only for the call's duration).
 * @pose: The capture-time pose joined from the pose ring by frame_id, or NULL
 *        if the message carried no frame_id or the frame had already aged out
 *        of the ring (detection arrived too late to aim on).
 * @user: The opaque pointer passed to orin_receiver_start().
 */
typedef void (*OrinDetectionHandler)(const OrinDetectionMsg *msg,
                                     const PoseEntry *pose,
                                     void *user);

/**
 * orin_receiver_start - Bind the PULL socket and start the receive thread.
 *
 * @endpoint: ZeroMQ bind endpoint, e.g. "tcp://0.0.0.0:5556".
 * @ring:     Pose ring to join frame_id -> pose against. Borrowed, not owned;
 *            must outlive the receiver.
 * @handler:  Called for each parsed message (see threading contract above).
 * @user:     Opaque pointer forwarded to the handler.
 *
 * Returns an opaque handle on success, or NULL on failure (bad args, zmq
 * setup failure, bind failure, or thread spawn failure — fully cleaned up).
 */
OrinReceiver *orin_receiver_start(const char *endpoint, PoseRing *ring,
                                  OrinDetectionHandler handler, void *user);

/**
 * orin_receiver_stop - Stop the receive thread and free all resources.
 * @r: Receiver handle. May be NULL (no-op).
 */
void orin_receiver_stop(OrinReceiver *r);

/**
 * orin_receiver_stats - Read the diagnostic counters (monotonic since start).
 * @r:              Receiver handle. NULL is a no-op.
 * @received:       Messages received. May be NULL.
 * @parse_failures: Messages that failed to parse. May be NULL.
 * @pose_misses:    Messages with no matching pose. May be NULL.
 *
 * Read racily off the receive thread: fine for reporting, not for control.
 */
void orin_receiver_stats(const OrinReceiver *r,
                         unsigned long *received,
                         unsigned long *parse_failures,
                         unsigned long *pose_misses);

#endif /* ORIN_RECEIVER_H */
