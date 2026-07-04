/**
 * rtp_stream_demo.c
 *
 * Stream live H.265 + per-frame frame_id SEI over RTP/UDP to the Orin. This is
 * the real transport path (frame_sender RTP_UDP sink -> rtph265pay -> udpsink),
 * driven by the real camera (libcamerasrc) by default.
 *
 * Build/run:
 *   make orin_rtp_stream
 *   ./orin_rtp_stream [host] [port] [seconds] [source]
 *     host    : Orin IP            (default 10.42.0.2)
 *     port    : Orin UDP port      (default 5600)
 *     seconds : run time, 0 = until Ctrl-C (default 0)
 *     source  : "camera" | "test"  (default camera; "test" = videotestsrc)
 *
 * Orin receive side (matching caps), for reference:
 *   gst-launch-1.0 -v udpsrc port=5600 \
 *     caps="application/x-rtp,media=video,encoding-name=H265,clock-rate=90000,payload=96" \
 *     ! rtpjitterbuffer ! rtph265depay ! h265parse ! nvv4l2decoder ! ...
 */

#include "frame_sender.h"

#include <gst/gst.h>
#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    GMutex   mtx;
    unsigned captured;
    guint32  last_id;
} Stats;

static GMainLoop *g_loop = NULL;

static void on_captured(uint32_t frame_id, void *user)
{
    Stats *st = (Stats *)user;
    g_mutex_lock(&st->mtx);
    st->captured++;
    st->last_id = frame_id;
    if ((frame_id % 30u) == 1u)   /* heartbeat ~once per second */
    {
        g_print("[RTP] captured frame_id=%u\n", frame_id);
    }
    g_mutex_unlock(&st->mtx);
}

static gboolean on_sigint(gpointer user)
{
    (void)user;
    g_print("\n[RTP] SIGINT — stopping...\n");
    if (g_loop) g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

static gboolean on_timeout(gpointer user)
{
    (void)user;
    if (g_loop) g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);

    const char *host    = (argc > 1) ? argv[1] : "10.42.0.2";
    int         port    = (argc > 2) ? atoi(argv[2]) : 5600;
    int         seconds = (argc > 3) ? atoi(argv[3]) : 0;
    const char *srcname = (argc > 4) ? argv[4] : "camera";

    Stats st;
    memset(&st, 0, sizeof(st));
    g_mutex_init(&st.mtx);

    FrameSenderConfig cfg;
    frame_sender_config_default(&cfg);
    cfg.source = (strcmp(srcname, "test") == 0)
                     ? FRAME_SENDER_SOURCE_VIDEOTEST
                     : FRAME_SENDER_SOURCE_LIBCAMERA;
    cfg.sink              = FRAME_SENDER_SINK_RTP_UDP;
    cfg.host              = host;
    cfg.port              = port;
    cfg.width             = 1920;
    cfg.height            = 1080;
    cfg.fps               = 30;
    cfg.bitrate_kbps      = 20000;
    cfg.key_int_max       = 30;
    cfg.speed_preset      = "superfast";
    cfg.on_frame_captured = on_captured;
    cfg.user              = &st;

    /* Optional H.264 app-preview branch, driven by env vars for measurement:
     *   APP_FIFO=<path>  APP_W APP_H APP_FPS APP_PRESET APP_BR(kbps) */
    const char *app_fifo = getenv("APP_FIFO");
    if (app_fifo && app_fifo[0])
    {
        cfg.app_preview      = true;
        cfg.app_fifo_path    = app_fifo;
        cfg.app_width        = getenv("APP_W")   ? atoi(getenv("APP_W"))   : 0;
        cfg.app_height       = getenv("APP_H")   ? atoi(getenv("APP_H"))   : 0;
        cfg.app_fps          = getenv("APP_FPS") ? atoi(getenv("APP_FPS")) : 0;
        cfg.app_bitrate_kbps = getenv("APP_BR")  ? atoi(getenv("APP_BR"))  : 8000;
        cfg.app_speed_preset = getenv("APP_PRESET") ? getenv("APP_PRESET") : "ultrafast";
        g_print("[RTP] app-preview H.264 -> %s (%s)\n", app_fifo, cfg.app_speed_preset);
    }

    g_print("[RTP] streaming %s 1080p30 H.265+SEI -> rtp://%s:%d  (%s)\n",
            cfg.source == FRAME_SENDER_SOURCE_VIDEOTEST ? "videotest" : "camera",
            host, port,
            seconds > 0 ? g_strdup_printf("%d s", seconds) : "until Ctrl-C");

    FrameSender *s = frame_sender_start(&cfg);
    if (s == NULL)
    {
        fprintf(stderr, "FAIL: frame_sender_start returned NULL "
                        "(camera busy? try source 'test')\n");
        return 1;
    }

    g_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_sigint, NULL);
    if (seconds > 0)
    {
        g_timeout_add_seconds((guint)seconds, on_timeout, NULL);
    }
    g_main_loop_run(g_loop);

    frame_sender_stop(s);
    g_main_loop_unref(g_loop);

    g_mutex_lock(&st.mtx);
    g_print("[RTP] done: %u frames sent, last frame_id=%u\n",
            st.captured, st.last_id);
    g_mutex_unlock(&st.mtx);
    return 0;
}
