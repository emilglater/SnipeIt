/**
 * frame_sender.c
 *
 * GStreamer implementation of the Pi->Orin H.265 sender. See frame_sender.h.
 */

#include "frame_sender.h"
#include "sei_frame_id.h"

#include <string.h>
#include <gst/gst.h>

struct FrameSender
{
    GstElement     *pipeline;
    GstElement     *capture_point;  /* identity 'cappoint' — capture probe pad */
    GstElement     *parse;          /* h265parse 'parse'   — SEI probe pad      */
    gulong          cap_probe_id;
    gulong          sei_probe_id;

    /* frame_id handoff: the capture probe pushes the id it assigned; the SEI
     * probe pops it. bframes=0 keeps strict order, so this FIFO stays aligned
     * with the encoder's 1:1 in-order output. */
    GAsyncQueue    *fid_queue;

    gint            next_fid;       /* atomic; first id handed out is 1         */
    volatile gint   frame_count;    /* atomic                                   */
    volatile gint   sei_calls;      /* atomic; SEI-probe invocations (diag)     */

    FrameCapturedFn on_captured;
    FrameSentFn     on_sent;
    void           *user;

    GMainLoop      *loop;
    GThread        *loop_thread;
    GstBus         *bus;
    guint           bus_watch_id;
    guint           drain_timeout_id;
};

/* ---- AU helpers ---------------------------------------------------------- */

/* Offset of the start code of the first VCL NAL (HEVC type 0..31) in an
 * Annex-B access unit, or 0 if none found (then we insert at the front). */
static gsize first_vcl_offset(const guint8 *d, gsize n)
{
    gsize i = 0;
    while (i + 4 <= n)
    {
        gsize hdr = 0;
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
        {
            hdr = i + 3;
        }
        else if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1)
        {
            hdr = i + 4;
        }
        else
        {
            i++;
            continue;
        }
        if (hdr < n)
        {
            int t = (d[hdr] >> 1) & 0x3F;
            if (t <= 31)
            {
                return i;   /* start code offset of the first VCL NAL */
            }
        }
        i = hdr;            /* skip past this start code, keep scanning */
    }
    return 0;
}

/* ---- Pad probes ---------------------------------------------------------- */

static GstPadProbeReturn cap_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                      gpointer user)
{
    (void)pad;
    FrameSender *s = (FrameSender *)user;
    GstBuffer   *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buf == NULL)
    {
        return GST_PAD_PROBE_OK;
    }

    guint32 fid = (guint32)g_atomic_int_add(&s->next_fid, 1);
    if (s->parse != NULL)   /* no SEI consumer when the Orin branch is off */
    {
        g_async_queue_push(s->fid_queue, GUINT_TO_POINTER(fid)); /* fid >= 1 -> non-NULL */
    }

    g_atomic_int_inc(&s->frame_count);

    if (s->on_captured != NULL)
    {
        s->on_captured(fid, s->user);
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn sei_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                      gpointer user)
{
    (void)pad;
    FrameSender *s = (FrameSender *)user;
    GstBuffer   *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buf == NULL)
    {
        return GST_PAD_PROBE_OK;
    }

    g_atomic_int_inc(&s->sei_calls);

    gpointer p = g_async_queue_try_pop(s->fid_queue);
    if (p == NULL)
    {
        return GST_PAD_PROBE_OK;   /* no id queued for this AU — pass through */
    }
    guint32 fid = GPOINTER_TO_UINT(p);

    uint8_t sei[SEI_FRAME_ID_MAX_BYTES];
    int     sn = sei_frame_id_encode(fid, sei, sizeof(sei));
    if (sn <= 0)
    {
        return GST_PAD_PROBE_OK;
    }

    GstMapInfo m;
    if (!gst_buffer_map(buf, &m, GST_MAP_READ))
    {
        return GST_PAD_PROBE_OK;
    }

    gsize off = first_vcl_offset(m.data, m.size);

    GstBuffer *nb = gst_buffer_new_allocate(NULL, m.size + (gsize)sn, NULL);
    GstMapInfo nm;
    gst_buffer_map(nb, &nm, GST_MAP_WRITE);
    memcpy(nm.data,             m.data,       off);              /* pre-VCL NALs */
    memcpy(nm.data + off,       sei,          (gsize)sn);        /* our SEI      */
    memcpy(nm.data + off + sn,  m.data + off, m.size - off);     /* VCL onward   */

    if (s->on_sent != NULL)
    {
        s->on_sent(nm.data, m.size + (gsize)sn, s->user);   /* still mapped */
    }

    gst_buffer_unmap(nb, &nm);
    gst_buffer_unmap(buf, &m);

    /* Carry timestamps/flags over; do NOT copy memory (we wrote our own). */
    gst_buffer_copy_into(nb, buf,
                         GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_FLAGS,
                         0, (gsize)-1);

    /* Replace the buffer travelling through the probe. */
    GST_PAD_PROBE_INFO_DATA(info) = nb;
    gst_buffer_unref(buf);
    return GST_PAD_PROBE_OK;
}

/* ---- Bus / loop ---------------------------------------------------------- */

static gboolean bus_cb(GstBus *bus, GstMessage *msg, gpointer user)
{
    (void)bus;
    FrameSender *s = (FrameSender *)user;
    switch (GST_MESSAGE_TYPE(msg))
    {
    case GST_MESSAGE_ERROR:
    {
        GError *err = NULL;
        gchar  *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        g_printerr("[SENDER] ERROR from %s: %s (%s)\n",
                   GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
        g_clear_error(&err);
        g_free(dbg);
        if (s->loop) g_main_loop_quit(s->loop);
        break;
    }
    case GST_MESSAGE_EOS:
        g_print("[SENDER] EOS\n");
        if (s->loop) g_main_loop_quit(s->loop);
        break;
    default:
        break;
    }
    return TRUE;
}

static gpointer loop_thread_fn(gpointer user)
{
    FrameSender *s = (FrameSender *)user;
    g_main_loop_run(s->loop);
    return NULL;
}

/* Fallback to quit the loop if EOS never arrives during drain. */
static gboolean drain_timeout_cb(gpointer user)
{
    FrameSender *s = (FrameSender *)user;
    s->drain_timeout_id = 0;
    if (s->loop) g_main_loop_quit(s->loop);
    return G_SOURCE_REMOVE;
}

/* ---- Pipeline build ------------------------------------------------------ */

static void build_sink_chain(const FrameSenderConfig *cfg, GString *p)
{
    switch (cfg->sink)
    {
    case FRAME_SENDER_SINK_RTP_UDP:
        g_string_append_printf(p,
            " ! rtph265pay config-interval=1 pt=96"
            " ! udpsink host=%s port=%d sync=false async=false",
            cfg->host ? cfg->host : "127.0.0.1", cfg->port);
        break;
    case FRAME_SENDER_SINK_RTSP:
        g_string_append_printf(p,
            " ! rtspclientsink location=%s",
            cfg->host ? cfg->host : "rtsp://127.0.0.1:8554/stream");
        break;
    case FRAME_SENDER_SINK_FAKE:
    default:
        g_string_append(p, " ! fakesink sync=false async=false");
        break;
    }
}

void frame_sender_config_default(FrameSenderConfig *cfg)
{
    if (cfg == NULL) return;
    /* Zero first, then set the non-zero defaults. */
    memset(cfg, 0, sizeof(*cfg));
    cfg->source        = FRAME_SENDER_SOURCE_LIBCAMERA;
    cfg->width         = 1920;
    cfg->height        = 1080;
    cfg->fps           = 30;
    cfg->bitrate_kbps  = 20000;          /* spend bits on small-target detail  */
    cfg->key_int_max   = 30;             /* frames, NOT seconds - see below    */
    cfg->speed_preset  = "superfast";
    /* Both are library defaults only; src/main.c overrides them. keyint counts
     * FRAMES: at the ~2.4-3.4 fps this software encode actually sustains,
     * keyint=30 is a 9-12 s blind window for a mid-stream joiner. */
    cfg->x265_extra    = NULL;
    cfg->orin_branch   = true;
    cfg->sink          = FRAME_SENDER_SINK_RTP_UDP;
    cfg->host          = NULL;
    cfg->port          = 5600;
    cfg->app_preview       = false;
    cfg->app_bitrate_kbps  = 8000;
    cfg->app_speed_preset  = "ultrafast";
    cfg->on_frame_captured = NULL;
    cfg->on_frame_sent     = NULL;
    cfg->user          = NULL;
}

FrameSender *frame_sender_start(const FrameSenderConfig *cfg)
{
    if (cfg == NULL)
    {
        return NULL;
    }

    FrameSender *s = g_new0(FrameSender, 1);
    s->fid_queue   = g_async_queue_new();
    s->next_fid    = 1;
    s->on_captured = cfg->on_frame_captured;
    s->on_sent     = cfg->on_frame_sent;
    s->user        = cfg->user;

    /* option-string carries x265 params the element has no property for.
     * bframes=0 keeps output 1:1 and in order, which is what keeps the
     * frame_id FIFO aligned with the access units; extra appended after. */
    GString *opt = g_string_new("bframes=0");
    if (cfg->x265_extra && cfg->x265_extra[0])
    {
        g_string_append_printf(opt, ":%s", cfg->x265_extra);
    }

    /* Source head -> a normalised I420 WxH@fps stream feeding 'cappoint'.
     *  - videotestsrc already produces video/x-raw, so a plain caps filter
     *    negotiates (this is the 68/68-validated path).
     *  - libcamerasrc advertises the IMX477 sensor modes (e.g. 2028x1080), not
     *    our exact 1920x1080, so we must videoscale/videorate/videoconvert to
     *    bridge to the requested size/rate/format; a direct caps filter on its
     *    src pad fails to negotiate (not-negotiated -4).
     *  - The framerate cap must reach libcamerasrc itself (videoconvert/
     *    videoscale forward it upstream): it becomes a FrameDurationLimits
     *    control that caps the auto-exposure shutter time. Without it, a dark
     *    scene lets AE stretch exposures to seconds (~0.1 fps) and starves
     *    every branch — black app stream, no RTP to the Orin.
     *  - NO videorate anywhere near the source. The camera's first frame
     *    arrives ~0.8 s into the segment (AGC warm-up); videorate gap-fills
     *    from t=0 with dozens of duplicate refs of the first camera buffer,
     *    parking libcamerasrc's tiny (~4) buffer pool behind the encoders and
     *    stalling capture at <1 fps (measured). The caps filter alone is
     *    enough to hold the source rate. */
    /* 1. Source head -> a normalised I420 WxH@fps stream (no cappoint here). */
    GString *p = g_string_new(NULL);
    if (cfg->source == FRAME_SENDER_SOURCE_VIDEOTEST)
    {
        g_string_append_printf(p,
            "videotestsrc is-live=true"
            " ! video/x-raw,width=%d,height=%d,framerate=%d/1"
            " ! videoconvert ! video/x-raw,format=I420",
            cfg->width, cfg->height, cfg->fps);
    }
    else
    {
        g_string_append_printf(p,
            "libcamerasrc"
            " ! videoconvert ! videoscale"
            " ! video/x-raw,format=I420,width=%d,height=%d,framerate=%d/1",
            cfg->width, cfg->height, cfg->fps);
    }

    /* 2. Optional tee: when the app-preview H.264 branch is on, split the
     *    capture. Both branches use a leaky queue so a slow software encoder
     *    drops its own frames instead of back-pressuring the source (and thus
     *    the other branch). max-size-buffers=1 is load-bearing: the branches
     *    hold refs to libcamerasrc's OWN capture buffers (head is passthrough)
     *    and its pool only has ~4 — queues of 4 park the whole pool behind the
     *    slow x265 branch and stall capture at <1 fps (measured); queues of 1
     *    park at most one per branch. 'cappoint' — where the frame_id is
     *    assigned and the pose recorded — sits in the H.265/Orin branch AFTER
     *    its leaky queue, so dropped frames never desync the frame_id<->AU
     *    mapping. */
    bool app_on = cfg->app_preview && cfg->app_fifo_path && cfg->app_fifo_path[0];
    if (!cfg->orin_branch && !app_on)
    {
        g_printerr("[SENDER] nothing to build: orin_branch off and no app preview\n");
        g_string_free(opt, TRUE);
        g_string_free(p, TRUE);
        g_async_queue_unref(s->fid_queue);
        g_free(s);
        return NULL;
    }
    if (cfg->orin_branch && app_on)
    {
        g_string_append(p, " ! tee name=dtee");
        /* H.265 / Orin branch */
        g_string_append(p,
            " dtee. ! queue max-size-buffers=1 leaky=downstream"
            " ! identity name=cappoint silent=true");
    }
    else
    {
        /* Single-branch mode (either encoder alone): no tee. cappoint still
         * assigns frame_ids and fires the pose callback. NOTE: here cappoint
         * sits BEFORE the leaky queue, the reverse of the dual-branch case
         * above, so frame_ids get assigned to frames the queue may later drop.
         * Harmless only because this mode has no SEI consumer; do not add one
         * without moving cappoint after the queue. */
        g_string_append(p, " ! identity name=cappoint silent=true");
    }

    /* 3. H.265 + SEI -> Orin (the tail of the cappoint branch). */
    if (cfg->orin_branch)
    {
        g_string_append_printf(p,
            " ! x265enc name=enc tune=zerolatency speed-preset=%s bitrate=%d"
            " key-int-max=%d option-string=\"%s\""
            " ! video/x-h265,stream-format=byte-stream,alignment=au"
            " ! h265parse name=parse",
            cfg->speed_preset ? cfg->speed_preset : "superfast",
            cfg->bitrate_kbps, cfg->key_int_max, opt->str);
        build_sink_chain(cfg, p);
    }
    g_string_free(opt, TRUE);

    /* 4. H.264 app-preview branch -> the FIFO the app's FFmpeg reads. */
    if (app_on)
    {
        int aw = cfg->app_width  > 0 ? cfg->app_width  : cfg->width;
        int ah = cfg->app_height > 0 ? cfg->app_height : cfg->height;
        int af = cfg->app_fps    > 0 ? cfg->app_fps    : cfg->fps;
        int abr = cfg->app_bitrate_kbps > 0 ? cfg->app_bitrate_kbps : 8000;
        const char *apreset = cfg->app_speed_preset ? cfg->app_speed_preset
                                                     : "ultrafast";
        if (cfg->orin_branch)
            g_string_append(p,
                " dtee. ! queue max-size-buffers=1 leaky=downstream");
        else
            /* No tee: continue inline after cappoint, same leaky decoupling. */
            g_string_append(p,
                " ! queue max-size-buffers=1 leaky=downstream");
        /* Scale/rate elements only when the preview actually differs from the
         * capture — every extra transform in this branch handles refs of the
         * camera's scarce pool buffers. videorate is drop-only + skip-to-first:
         * it must never DUPLICATE (gap-filling floods the branch with refs of
         * one camera buffer — the <1 fps stall) and must not fill from t=0. */
        if (aw != cfg->width || ah != cfg->height)
            g_string_append_printf(p,
                " ! videoscale ! video/x-raw,width=%d,height=%d", aw, ah);
        if (af != cfg->fps)
            g_string_append_printf(p,
                " ! videorate drop-only=true skip-to-first=true"
                " ! video/x-raw,framerate=%d/1", af);
        g_string_append_printf(p,
            " ! x264enc tune=zerolatency speed-preset=%s bitrate=%d key-int-max=%d"
            " ! video/x-h264,stream-format=byte-stream,alignment=au"
            /* config-interval=1: repeat SPS/PPS ~every second so FFmpeg's raw-H.264
             * probe locks on no matter when it starts reading the FIFO. */
            " ! h264parse config-interval=1 ! filesink location=%s sync=false async=false",
            apreset, abr, af, cfg->app_fifo_path);
    }

    GError *err = NULL;
    s->pipeline = gst_parse_launch(p->str, &err);
    if (s->pipeline == NULL)
    {
        g_printerr("[SENDER] pipeline build failed: %s\n",
                   err ? err->message : "(unknown)");
        g_clear_error(&err);
        g_string_free(p, TRUE);
        g_async_queue_unref(s->fid_queue);
        g_free(s);
        return NULL;
    }
    if (err)   /* non-fatal parse warning */
    {
        g_clear_error(&err);
    }
    g_string_free(p, TRUE);

    s->capture_point = gst_bin_get_by_name(GST_BIN(s->pipeline), "cappoint");
    s->parse         = gst_bin_get_by_name(GST_BIN(s->pipeline), "parse");
    if (s->capture_point == NULL || (cfg->orin_branch && s->parse == NULL))
    {
        g_printerr("[SENDER] could not find probe elements\n");
        frame_sender_stop(s);
        return NULL;
    }

    GstPad *cap_pad = gst_element_get_static_pad(s->capture_point, "src");
    /* INVARIANT: splice the SEI on h265parse's SINK pad (x265enc output,
     * byte-stream/au), never its src pad. Post-parse insertion is not
     * re-validated, and rtph265pay then mis-frames the AU boundaries so no
     * depayloader can reassemble the stream - even though the same bytes decode
     * fine as an elementary stream. */
    s->cap_probe_id = gst_pad_add_probe(cap_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                        cap_probe_cb, s, NULL);
    gst_object_unref(cap_pad);
    if (s->parse)
    {
        GstPad *sei_pad = gst_element_get_static_pad(s->parse, "sink");
        s->sei_probe_id = gst_pad_add_probe(sei_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                            sei_probe_cb, s, NULL);
        gst_object_unref(sei_pad);
    }

    /* Bus watch on a private main loop/thread so errors are surfaced. */
    s->loop          = g_main_loop_new(NULL, FALSE);
    s->bus           = gst_element_get_bus(s->pipeline);
    s->bus_watch_id  = gst_bus_add_watch(s->bus, bus_cb, s);
    s->loop_thread   = g_thread_new("sender-bus", loop_thread_fn, s);

    if (gst_element_set_state(s->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("[SENDER] failed to set PLAYING\n");
        frame_sender_stop(s);
        return NULL;
    }

    g_print("[SENDER] streaming %dx%d@%d %s (%s) -> sink\n",
            cfg->width, cfg->height, cfg->fps,
            cfg->orin_branch ? "H.265" : "H.264 app-only (Orin branch OFF)",
            cfg->source == FRAME_SENDER_SOURCE_VIDEOTEST ? "videotest" : "libcamera");
    return s;
}

void frame_sender_stop(FrameSender *s)
{
    if (s == NULL)
    {
        return;
    }

    /* Graceful drain: push EOS so x265enc flushes its in-flight frames through
     * the SEI probe and the sink, instead of dropping them on the NULL
     * transition. bus_cb quits the loop on EOS; a timeout guards against EOS
     * never arriving. */
    if (s->pipeline && s->loop_thread)
    {
        s->drain_timeout_id = g_timeout_add(2500, drain_timeout_cb, s);
        gst_element_send_event(s->pipeline, gst_event_new_eos());
        g_thread_join(s->loop_thread);
        s->loop_thread = NULL;
        if (s->drain_timeout_id)
        {
            g_source_remove(s->drain_timeout_id);
            s->drain_timeout_id = 0;
        }
    }

    if (s->pipeline)
    {
        gst_element_set_state(s->pipeline, GST_STATE_NULL);
    }
    if (s->loop)
    {
        g_main_loop_quit(s->loop);
    }
    if (s->loop_thread)
    {
        g_thread_join(s->loop_thread);
    }
    if (s->bus_watch_id)
    {
        g_source_remove(s->bus_watch_id);
    }
    if (s->bus)
    {
        gst_object_unref(s->bus);
    }
    if (s->loop)
    {
        g_main_loop_unref(s->loop);
    }
    if (s->capture_point)
    {
        gst_object_unref(s->capture_point);
    }
    if (s->parse)
    {
        gst_object_unref(s->parse);
    }
    if (s->pipeline)
    {
        gst_object_unref(s->pipeline);
    }

    g_print("[SENDER] stopped (captured=%d sei_probe_calls=%d, %d ids undrained)\n",
            g_atomic_int_get(&s->frame_count),
            g_atomic_int_get(&s->sei_calls),
            s->fid_queue ? g_async_queue_length(s->fid_queue) : 0);

    if (s->fid_queue)
    {
        g_async_queue_unref(s->fid_queue);
    }
    g_free(s);
}

unsigned long frame_sender_frame_count(const FrameSender *s)
{
    if (s == NULL)
    {
        return 0;
    }
    return (unsigned long)g_atomic_int_get(&((FrameSender *)s)->frame_count);
}
