/**
 * frame_sender.h
 *
 * The Pi->Orin H.265 frame sender.
 *
 * Pipeline (GStreamer):
 * libcamerasrc -> convert/scale -> [tee] -> queue -> cappoint -> x265enc
 * -> h265parse -> RTP/UDP (Orin) -> queue -> x264enc -> FIFO (app)
 * This Pi (Pi 5) has no hardware video encoder, so both encoders are software.
 * The H.265 side is tuned zerolatency, no B-frames, short GOP, bitrate biased
 * for small-target detail.
 *
 * Two things happen per frame, via pad probes inside the .c:
 *   1. At CAPTURE, the raw frame is assigned a monotonic frame_id and the
 *      on_frame_captured callback fires so the caller can record the
 *      capture-time servo pose (ddl_bridge_record_capture_pose).
 *   2. After encoding, our frame_id SEI NAL (sei_frame_id) is spliced into the
 *      access unit just before the first VCL NAL, so the Orin can read it with
 *      any HEVC parser and echo it back with its detections.
 *   The two are matched by a FIFO queue, not by timestamp: the capture probe
 *   pushes each id, the SEI probe pops one per access unit. bframes=0 and
 *   alignment=au keep x265enc's output 1:1 and in order, which is what keeps
 *   the FIFO aligned. If that ever stops holding, the ids shift by one and
 *   never recover - frame_sender_stop() logs any undrained ids.
 *
 * This header is intentionally free of <gst/gst.h> so it can be included from
 * ddl_bridge without dragging GStreamer into every translation unit.
 */

#ifndef ORIN_FRAME_SENDER_H
#define ORIN_FRAME_SENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct FrameSender FrameSender;

typedef enum
{
    FRAME_SENDER_SOURCE_LIBCAMERA,  /* real IMX477 via libcamerasrc        */
    FRAME_SENDER_SOURCE_VIDEOTEST   /* videotestsrc — camera-less testing  */
} FrameSenderSourceType;

typedef enum
{
    FRAME_SENDER_SINK_RTP_UDP,  /* RTP/H265 over UDP to host:port (direct link) */
    FRAME_SENDER_SINK_RTSP,     /* publish to an RTSP URL via rtspclientsink    */
    FRAME_SENDER_SINK_FAKE      /* fakesink — testing, no transport             */
} FrameSenderSinkType;

/* Called on the capture thread when a raw frame is assigned a frame_id, BEFORE
 * it is encoded. The caller records the capture-time pose here. May be NULL. */
typedef void (*FrameCapturedFn)(uint32_t frame_id, void *user);

/* Optional diagnostic hook: the fully-tagged Annex-B access unit (SEI + AU) as
 * it leaves the parser, post-splice. Used by the demo/test to verify the SEI
 * round-trips. May be NULL. The bytes are valid only for the call. */
typedef void (*FrameSentFn)(const uint8_t *au, size_t len, void *user);

typedef struct
{
    FrameSenderSourceType source;
    int          width;
    int          height;
    int          fps;

    int          bitrate_kbps;    /* x265 target bitrate                       */
    int          key_int_max;     /* GOP length (short -> fast decode start)    */
    const char  *speed_preset;    /* "ultrafast".."medium": Pi5 CPU vs quality */
    const char  *x265_extra;      /* extra option-string, e.g. "rc-lookahead=10" */

    /* false: build NO H.265/Orin branch at all — single x264 encoder feeding
     * the app preview (requires app_preview). cappoint (frame_id + pose
     * callback) still runs; there is just no SEI/RTP consumer. For isolating
     * the app stream from the dual-encoder load. */
    bool         orin_branch;

    FrameSenderSinkType sink;
    const char  *host;            /* RTP: Orin IP. RTSP: full rtsp:// URL.      */
    int          port;            /* RTP UDP port.                              */

    /* Optional H.264 "app preview" branch. When enabled, a tee off the same
     * capture feeds a software x264 encode to app_fifo_path (the camera FIFO
     * the existing FFmpeg->mediaMTX->app path reads). It runs on its own leaky
     * queue, decoupled from the H.265/Orin branch, so the two software encoders
     * don't rate-couple. bbox coords stay 1920x1080 regardless of app_* size,
     * so the app overlay is unaffected if the preview is downscaled. */
    bool         app_preview;         /* enable the H.264 branch                 */
    const char  *app_fifo_path;       /* filesink target (camera FIFO / file)    */
    int          app_width;           /* 0 -> same as width                      */
    int          app_height;          /* 0 -> same as height                     */
    int          app_fps;             /* 0 -> same as fps                        */
    int          app_bitrate_kbps;    /* x264 target bitrate (default 8000)      */
    const char  *app_speed_preset;    /* x264 preset (default "ultrafast")       */

    FrameCapturedFn on_frame_captured;
    FrameSentFn     on_frame_sent;
    void           *user;
} FrameSenderConfig;

/**
 * @brief Fill cfg with LIBRARY defaults: 1080p capture capped at 30 fps,
 *        zerolatency/no-B, libcamera source, RTP/UDP sink.
 *
 * @param cfg Configuration to populate.
 *
 * @details These are starting values, not the running configuration: the
 *          service in src/main.c overrides speed_preset, key_int_max and
 *          x265_extra. The 30 fps is the CAPTURE cap; the software x265 encode
 *          delivers ~2.4-3.4 fps to the Orin on this Pi.
 */
void frame_sender_config_default(FrameSenderConfig *cfg);

/**
 * @brief Build and start the pipeline.
 *
 * @param cfg Read during this call only; nothing is retained, so the strings
 *            need not outlive the call.
 *
 * @details gst_init() must have been called by the process beforehand.
 *
 * @returns A handle, or NULL on failure (logged).
 */
FrameSender *frame_sender_start(const FrameSenderConfig *cfg);

/**
 * @brief Stop the pipeline and free the sender.
 *
 * @param s The sender. May be NULL (no-op).
 */
void frame_sender_stop(FrameSender *s);

/**
 * @brief Total frames captured since start (diagnostic).
 *
 * @param s The sender.
 *
 * @returns The running total of captured frames.
 */
unsigned long frame_sender_frame_count(const FrameSender *s);

#endif /* ORIN_FRAME_SENDER_H */
