/**
 * detection_msg.h
 *
 * Parsed representation of a detection message coming back from the Orin,
 * plus a focused JSON parser for it.
 *
 * Wire format: one JSON object per ZeroMQ message. There is no line framing -
 * the zmq message boundary IS the message boundary; the parser takes ptr+len
 * and never looks for a newline. The schema matches the "target_detection"
 * shape the app already consumes, with a
 * top-level "frame_id" added so the Pi can join the detection back to the
 * capture-time servo pose via the pose ring:
 *
 *   {
 *     "type": "target_detection",
 *     "frame_id": 12345,
 *     "timestamp_ms": 678,
 *     "detections": [
 *       { "id": "1", "class": "HUMAN", "confidence": 0.85,
 *         "bbox": {"x": 100, "y": 50, "width": 200, "height": 400} }
 *     ]
 *   }
 *
 * bbox coordinates MUST be pixels in the 1920x1080 source frame, with any
 * detector letterbox/resize already reversed on the Orin. The Pi's aiming
 * geometry normalises against exactly that size (aiming.h note 1) and the app
 * overlay consumes the same pixels, so this is a shared contract - not an
 * Orin-side detail. The Pi turns bbox + pose + FOV + known target height into
 * an aiming angle.
 *
 * This translation unit deliberately has NO ZeroMQ dependency so the parser
 * can be unit-tested on its own.
 */

#ifndef ORIN_DETECTION_MSG_H
#define ORIN_DETECTION_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hard caps keep the parsed message a fixed-size, stack-friendly POD and
 * bound the work done on any single (possibly malformed) input line. */
#define ORIN_MAX_DETECTIONS 32
#define ORIN_ID_MAXLEN      16
#define ORIN_CLASS_MAXLEN   16

typedef struct
{
    char  target_id[ORIN_ID_MAXLEN];  /* "id" - the Orin's PER-FRAME label.
                                       * NOT stable across frames; the Pi-side
                                       * tracker supplies stable ids. Used only
                                       * as a fallback on a pose-join miss.   */
    char  cls[ORIN_CLASS_MAXLEN];     /* "class" — e.g. "HUMAN".              */
    float confidence;                 /* "confidence" in [0,1].               */
    int   bbox_x;                     /* "bbox.x"      pixels, detector frame. */
    int   bbox_y;                     /* "bbox.y"                              */
    int   bbox_w;                     /* "bbox.width"                          */
    int   bbox_h;                     /* "bbox.height"                         */
} OrinDetection;

typedef struct
{
    bool          has_frame_id;       /* false if the message omitted frame_id. */
    uint32_t      frame_id;           /* join key into the pose ring.           */
    uint64_t      timestamp_ms;       /* Orin-side timestamp, informational.    */
    int           num_detections;     /* 0..ORIN_MAX_DETECTIONS.                */
    OrinDetection detections[ORIN_MAX_DETECTIONS];
} OrinDetectionMsg;

/**
 * orin_detection_msg_parse - Parse one JSON detection line into *out.
 *
 * @json: Pointer to the JSON text (need not be NUL-terminated).
 * @len:  Length of the JSON text in bytes.
 * @out:  Filled on success; zero-initialised by the function before parsing.
 *
 * Returns true if the line parsed as a well-formed object. Unknown keys are
 * skipped. Detections beyond ORIN_MAX_DETECTIONS are dropped (the rest still
 * parse). A missing "detections" array yields num_detections == 0 and still
 * returns true. Returns false only on structurally broken JSON.
 *
 * Pure function: no allocation, no globals, no I/O.
 */
bool orin_detection_msg_parse(const char *json, size_t len, OrinDetectionMsg *out);

#endif /* ORIN_DETECTION_MSG_H */
