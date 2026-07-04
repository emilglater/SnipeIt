/**
 * sei_capture_demo.c
 *
 * Produce a short raw H.265 elementary-stream capture WITH the real per-frame
 * frame_id SEI tagging, for the Orin side to validate its NVDEC-decode +
 * SEI-extraction path offline (before the live link exists).
 *
 * It runs the real frame_sender pipeline (x265enc -> h265parse -> [SEI splice])
 * and writes every fully-tagged Annex-B access unit, exactly as it would go on
 * the wire, to a .hevc file. videotestsrc is used as the pixel source because
 * the SEI tagging is identical regardless of source — the content is irrelevant
 * to SEI extraction; the encoder profile/level/format are representative of the
 * real 1080p stream.
 *
 * Build/run:  make orin_sei_capture && ./orin_sei_capture <out.hevc> [seconds]
 *
 * The resulting file:
 *   - starts with VPS/SPS/PPS (x265 emits them before the first IDR),
 *   - has, in every access unit, our prefix-SEI (type 39) spliced before the
 *     first VCL NAL, carrying the frame_id (1,2,3,... in order),
 *   - is a plain Annex-B byte stream: decodable by ffmpeg/NVDEC and parsable by
 *     sei_frame_id_parse / your own SEI scanner.
 */

#include "frame_sender.h"
#include "sei_frame_id.h"

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct
{
    FILE    *fp;
    GMutex   mtx;
    unsigned aus;
    unsigned tagged;
    guint32  first_id;
    guint32  last_id;
} Cap;

/* Each call hands us one fully-spliced Annex-B access unit (SEI + AU). Append
 * it verbatim; the concatenation is a valid elementary stream. */
static void on_sent(const uint8_t *au, size_t len, void *user)
{
    Cap     *c  = (Cap *)user;
    uint32_t id = 0;
    gboolean ok = sei_frame_id_parse(au, len, &id);

    g_mutex_lock(&c->mtx);
    fwrite(au, 1, len, c->fp);
    c->aus++;
    if (ok)
    {
        c->tagged++;
        if (c->first_id == 0) c->first_id = id;
        c->last_id = id;
    }
    g_mutex_unlock(&c->mtx);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <out.hevc> [seconds]\n", argv[0]);
        return 2;
    }
    const char *path    = argv[1];
    int         seconds = (argc > 2) ? atoi(argv[2]) : 5;
    if (seconds <= 0) seconds = 5;

    Cap c;
    memset(&c, 0, sizeof(c));
    g_mutex_init(&c.mtx);
    c.fp = fopen(path, "wb");
    if (c.fp == NULL)
    {
        fprintf(stderr, "FAIL: cannot open %s for writing\n", path);
        return 1;
    }

    FrameSenderConfig cfg;
    frame_sender_config_default(&cfg);
    cfg.source        = FRAME_SENDER_SOURCE_VIDEOTEST;  /* content irrelevant */
    cfg.sink          = FRAME_SENDER_SINK_FAKE;          /* we capture in on_sent */
    cfg.width         = 1920;                            /* representative profile/level */
    cfg.height        = 1080;
    cfg.fps           = 30;
    cfg.bitrate_kbps  = 20000;
    cfg.key_int_max   = 30;
    cfg.speed_preset  = "superfast";
    cfg.on_frame_sent = on_sent;
    cfg.user          = &c;

    FrameSender *s = frame_sender_start(&cfg);
    if (s == NULL)
    {
        fprintf(stderr, "FAIL: frame_sender_start returned NULL\n");
        fclose(c.fp);
        return 1;
    }

    g_print("[CAPTURE] writing %d s of 1080p30 H.265 + SEI to %s ...\n",
            seconds, path);
    g_usleep((gulong)seconds * G_USEC_PER_SEC);

    frame_sender_stop(s);
    fclose(c.fp);

    g_mutex_lock(&c.mtx);
    unsigned aus = c.aus, tagged = c.tagged;
    guint32  fi = c.first_id, la = c.last_id;
    g_mutex_unlock(&c.mtx);

    g_print("[CAPTURE] wrote %u access units, %u SEI-tagged (frame_id %u..%u) -> %s\n",
            aus, tagged, fi, la, path);

    if (aus == 0 || tagged == 0)
    {
        fprintf(stderr, "FAIL: nothing captured/tagged\n");
        return 1;
    }
    g_print("PASS: %s is a SEI-tagged H.265 elementary stream\n", path);
    return 0;
}
