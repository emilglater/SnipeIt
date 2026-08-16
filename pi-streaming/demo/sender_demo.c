/**
 * sender_demo.c
 *
 * Camera-less, network-less smoke test of the frame sender on this Pi:
 * videotestsrc -> videoconvert -> x265enc -> h265parse -> [SEI splice] ->
 * fakesink. It exercises the real GStreamer/x265 pipeline and verifies that
 * the frame_id assigned at capture round-trips through the encoded H.265
 * access unit via the SEI (parsed back in the on_frame_sent hook).
 *
 * Build/run:  make orin_sender_demo && ./orin_sender_demo [seconds]
 *
 * Exit code 0 = frames flowed and every tagged AU parsed back a frame_id with
 * no mismatches; non-zero otherwise.
 */

#include "frame_sender.h"
#include "sei_frame_id.h"

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct
{
    GMutex   mtx;
    unsigned captured;
    unsigned sent;        /* AUs seen at the sink                       */
    unsigned parsed_ok;   /* AUs whose SEI parsed back to a frame_id    */
    unsigned mismatch;    /* parsed id out of expected monotonic order  */
    guint32  last_id;
} Stats;

static void on_captured(uint32_t frame_id, void *user)
{
    Stats *st = (Stats *)user;
    (void)frame_id;
    g_mutex_lock(&st->mtx);
    st->captured++;
    g_mutex_unlock(&st->mtx);
}

static void on_sent(const uint8_t *au, size_t len, void *user)
{
    Stats   *st = (Stats *)user;
    uint32_t id = 0;
    gboolean ok = sei_frame_id_parse(au, len, &id);

    g_mutex_lock(&st->mtx);
    st->sent++;
    if (ok)
    {
        st->parsed_ok++;
        /* frame_ids are handed out 1,2,3,...; they must arrive strictly
         * increasing (bframes=0, in order). */
        if (st->last_id != 0 && id <= st->last_id)
        {
            st->mismatch++;
        }
        st->last_id = id;
    }
    g_mutex_unlock(&st->mtx);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);

    int seconds = (argc > 1) ? atoi(argv[1]) : 3;
    if (seconds <= 0) seconds = 3;

    Stats st;
    memset(&st, 0, sizeof(st));
    g_mutex_init(&st.mtx);

    FrameSenderConfig cfg;
    frame_sender_config_default(&cfg);
    cfg.source            = FRAME_SENDER_SOURCE_VIDEOTEST;  /* no camera needed */
    cfg.sink              = FRAME_SENDER_SINK_FAKE;          /* no network       */
    cfg.width             = 1280;
    cfg.height            = 720;
    cfg.fps               = 30;
    cfg.speed_preset      = "ultrafast";                     /* keep the test light */
    cfg.on_frame_captured = on_captured;
    cfg.on_frame_sent     = on_sent;
    cfg.user              = &st;

    FrameSender *s = frame_sender_start(&cfg);
    if (s == NULL)
    {
        fprintf(stderr, "FAIL: frame_sender_start returned NULL\n");
        return 1;
    }

    g_print("[DEMO] running for %d s...\n", seconds);
    g_usleep((gulong)seconds * G_USEC_PER_SEC);

    frame_sender_stop(s);

    g_mutex_lock(&st.mtx);
    unsigned captured = st.captured, sent = st.sent,
             parsed = st.parsed_ok, mismatch = st.mismatch;
    g_mutex_unlock(&st.mtx);

    g_print("[DEMO] captured=%u  AUs=%u  sei_parsed=%u  mismatches=%u\n",
            captured, sent, parsed, mismatch);

    int fail = 0;
    if (captured == 0)         { fprintf(stderr, "FAIL: no frames captured\n");        fail = 1; }
    if (parsed == 0)           { fprintf(stderr, "FAIL: no SEI parsed back\n");        fail = 1; }
    if (mismatch != 0)         { fprintf(stderr, "FAIL: %u id-order mismatches\n", mismatch); fail = 1; }
    /* Every access unit that reached the sink should carry our SEI. */
    if (sent > 0 && parsed < sent)
    {
        fprintf(stderr, "FAIL: %u/%u AUs missing SEI\n", sent - parsed, sent);
        fail = 1;
    }

    if (!fail)
    {
        g_print("PASS: frame_id SEI round-trips through the live H.265 pipeline\n");
    }
    return fail;
}
