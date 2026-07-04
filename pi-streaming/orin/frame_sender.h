/**
 * frame_sender.h
 *
 * The Pi->Orin H.265 frame sender (protocol work item 1).
 *
 * Pipeline (GStreamer): camera -> videoconvert -> x265enc -> h265parse ->
 * transport. This Pi (Pi 5) has no hardware video encoder, so x265enc is
 * software; the encoder is tuned per the protocol: zerolatency, no B-frames,
 * short GOP, quality biased for small-target detail.
 *
 * Two things happen per frame, via pad probes inside the .c:
 *   1. At CAPTURE, the raw frame is assigned a monotonic frame_id and the
 *      on_frame_captured callback fires so the caller can record the
 *      capture-time servo pose (ddl_bridge_record_capture_pose).
 *   2. After encoding, our frame_id SEI NAL (sei_frame_id) is spliced into the
 *      access unit just before the first VCL NAL, so the Orin can read it with
 *      any HEVC parser and echo it back with its detections.
 *   The two are matched by buffer PTS (x265enc preserves it; bframes=0 keeps
 *   strict order), so the SEI carries the id assigned at capture even though
 *   it is inserted at the encoder output.
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
 * frame_sender_config_default - Fill cfg with protocol defaults
 * (1080p30, zerolatency/no-B/short-GOP, libcamera source, RTP/UDP sink).
 */
void frame_sender_config_default(FrameSenderConfig *cfg);

/**
 * frame_sender_start - Build and start the pipeline.
 * @cfg: configuration (copied; strings must outlive only this call).
 * Returns a handle, or NULL on failure (logged). gst_init() must have been
 * called by the process beforehand.
 */
FrameSender *frame_sender_start(const FrameSenderConfig *cfg);

/**
 * frame_sender_stop - Stop the pipeline and free the sender. NULL-safe.
 */
void frame_sender_stop(FrameSender *s);

/**
 * frame_sender_frame_count - Total frames captured since start (diagnostic).
 */
unsigned long frame_sender_frame_count(const FrameSender *s);

#endif /* ORIN_FRAME_SENDER_H */
