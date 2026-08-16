/**
 * live_loop_demo.c
 *
 * First end-to-end Pi<->Orin live-loop harness, in ONE process so the sender
 * and the detection receiver share a single pose ring:
 *
 *   camera --[frame_sender: x265+SEI]--> RTP/UDP --> Orin
 *        \--on_frame_captured: pose_ring_record(frame_id, pose, t)
 *   Orin --[detect]--> ZMQ JSON --> orin_receiver(PULL bind)
 *        \--handler: pose_ring_lookup(frame_id) -> aim_compute -> print
 *
 * Why one process: the pose ring is shared in-memory between the sender (writer,
 * at capture) and the receiver (reader, on detection). The full service wires
 * this into ddl_bridge with REAL servo poses; here, with no DDL/servo stack
 * running (the camera can only be held once), we record a PLACEHOLDER capture
 * pose (--pan/--tilt, default 0,0). That still proves the whole loop: transport,
 * SEI frame_id echo, frame_id->pose join, and the aiming geometry. The pose
 * VALUES become real once this is wired into streaming_server.
 *
 * Build/run:
 *   make orin_live_loop
 *   ./orin_live_loop [orin_host] [rtp_port] [zmq_bind] [seconds] [camera|test]
 *     orin_host : RTP dest IP     (default 10.42.0.2)
 *     rtp_port  : RTP dest UDP    (default 5600)
 *     zmq_bind  : PULL bind       (default tcp://0.0.0.0:5556)
 *     seconds   : run time, 0 = until Ctrl-C (default 0)
 *     source    : camera | test   (default camera)
 */

#include "frame_sender.h"
#include "pose_ring.h"
#include "orin_receiver.h"
#include "detection_msg.h"
#include "aiming.h"

#include <gst/gst.h>
#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
    PoseRing  *ring;
    AimConfig  aim;
    float      pan, tilt;          /* placeholder capture pose (no servo stack) */

    GMutex     mtx;
    unsigned   captured;
    unsigned   msgs;
    unsigned   dets;
    unsigned   pose_hits;
    unsigned   pose_misses;
    guint32    last_capture_id;
} Ctx;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Sender thread: tag assigned, record the capture-time pose under that id. */
static void on_captured(uint32_t frame_id, void *user)
{
    Ctx *c = (Ctx *)user;
    pose_ring_record(c->ring, frame_id, c->pan, c->tilt, now_ms());
    g_mutex_lock(&c->mtx);
    c->captured++;
    c->last_capture_id = frame_id;
    g_mutex_unlock(&c->mtx);
}

/* Receiver thread: a detection message came back from the Orin. */
static void on_detection(const OrinDetectionMsg *msg, const PoseEntry *pose,
                         void *user)
{
    Ctx *c = (Ctx *)user;

    g_mutex_lock(&c->mtx);
    c->msgs++;
    c->dets += (unsigned)msg->num_detections;
    if (pose) c->pose_hits++; else c->pose_misses++;
    g_mutex_unlock(&c->mtx);

    if (msg->num_detections == 0)
    {
        g_print("[LOOP] frame_id=%u  detections=0  pose=%s\n",
                msg->frame_id, pose ? "JOINED" : "MISS");
        return;
    }

    /* Pick the highest-confidence detection (Pi owns tracking; until the
     * tracker is in, follow best confidence — exactly the service fallback). */
    int best = 0;
    for (int i = 1; i < msg->num_detections; i++)
    {
        if (msg->detections[i].confidence > msg->detections[best].confidence)
            best = i;
    }
    const OrinDetection *d = &msg->detections[best];

    if (pose == NULL)
    {
        g_print("[LOOP] frame_id=%u  best=%s conf=%.2f bbox=[%d,%d %dx%d]  "
                "pose=MISS (too stale to aim)\n",
                msg->frame_id, d->cls, d->confidence,
                d->bbox_x, d->bbox_y, d->bbox_w, d->bbox_h);
        return;
    }

    AimSolution sol;
    bool ok = aim_compute(&c->aim, d->bbox_x, d->bbox_y, d->bbox_w, d->bbox_h,
                          pose->hor_angle, pose->ver_angle, &sol);
    float dist = aim_estimate_distance_m(&c->aim, d->bbox_h, 1.7f /* HUMAN ~1.7m */);

    if (ok)
    {
        g_print("[LOOP] frame_id=%u  best=%s conf=%.2f bbox=[%d,%d %dx%d]  "
                "pose=(%.2f,%.2f)  -> aim pan=%.2f tilt=%.2f "
                "(off %.2f,%.2f%s)  ~%.1fm\n",
                msg->frame_id, d->cls, d->confidence,
                d->bbox_x, d->bbox_y, d->bbox_w, d->bbox_h,
                pose->hor_angle, pose->ver_angle,
                sol.pan_deg, sol.tilt_deg,
                sol.pan_offset_deg, sol.tilt_offset_deg,
                sol.clamped ? " CLAMPED" : "", dist);
    }
    else
    {
        g_print("[LOOP] frame_id=%u  aim_compute rejected the bbox\n",
                msg->frame_id);
    }
}

static GMainLoop *g_loop = NULL;
static gboolean on_sigint(gpointer u) { (void)u; if (g_loop) g_main_loop_quit(g_loop); return G_SOURCE_REMOVE; }
static gboolean on_timeout(gpointer u){ (void)u; if (g_loop) g_main_loop_quit(g_loop); return G_SOURCE_REMOVE; }

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);

    const char *host    = (argc > 1) ? argv[1] : "10.42.0.2";
    int         port    = (argc > 2) ? atoi(argv[2]) : 5600;
    const char *zmqbind = (argc > 3) ? argv[3] : "tcp://0.0.0.0:5556";
    int         seconds = (argc > 4) ? atoi(argv[4]) : 0;
    const char *srcname = (argc > 5) ? argv[5] : "camera";

    Ctx c;
    memset(&c, 0, sizeof(c));
    g_mutex_init(&c.mtx);
    c.pan = 90.0f;   /* placeholder home pose (mid servo travel) — no DDL stack */
    c.tilt = 90.0f;
    aim_config_default(&c.aim);      /* PROVISIONAL FOV until probe_camera_fov */

    c.ring = pose_ring_create(POSE_RING_DEFAULT_CAPACITY);
    if (c.ring == NULL)
    {
        fprintf(stderr, "FAIL: pose_ring_create\n");
        return 1;
    }

    /* Receiver first so we are bound and listening before any frame goes out. */
    OrinReceiver *rx = orin_receiver_start(zmqbind, c.ring, on_detection, &c);
    if (rx == NULL)
    {
        fprintf(stderr, "FAIL: orin_receiver_start(%s) — port busy?\n", zmqbind);
        pose_ring_destroy(c.ring);
        return 1;
    }
    g_print("[LOOP] PULL bound at %s ; aiming FOV=%.1f/%.1f deg @ %dx%d "
            "(PROVISIONAL), capture pose=(%.1f,%.1f placeholder)\n",
            zmqbind, c.aim.hfov_deg, c.aim.vfov_deg, c.aim.frame_w, c.aim.frame_h,
            c.pan, c.tilt);

    FrameSenderConfig cfg;
    frame_sender_config_default(&cfg);
    cfg.source            = (strcmp(srcname, "test") == 0)
                                ? FRAME_SENDER_SOURCE_VIDEOTEST
                                : FRAME_SENDER_SOURCE_LIBCAMERA;
    cfg.sink              = FRAME_SENDER_SINK_RTP_UDP;
    cfg.host              = host;
    cfg.port              = port;
    cfg.on_frame_captured = on_captured;
    cfg.user              = &c;

    FrameSender *s = frame_sender_start(&cfg);
    if (s == NULL)
    {
        fprintf(stderr, "FAIL: frame_sender_start (camera busy? try 'test')\n");
        orin_receiver_stop(rx);
        pose_ring_destroy(c.ring);
        return 1;
    }
    g_print("[LOOP] streaming %s 1080p H.265+SEI -> rtp://%s:%d  (%s)\n",
            cfg.source == FRAME_SENDER_SOURCE_VIDEOTEST ? "videotest" : "camera",
            host, port, seconds > 0 ? "timed" : "until Ctrl-C");

    g_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_sigint, NULL);
    if (seconds > 0) g_timeout_add_seconds((guint)seconds, on_timeout, NULL);
    g_main_loop_run(g_loop);

    frame_sender_stop(s);

    unsigned long rcv = 0, pf = 0, pm = 0;
    orin_receiver_stats(rx, &rcv, &pf, &pm);   /* read before teardown */
    orin_receiver_stop(rx);

    pose_ring_destroy(c.ring);
    g_main_loop_unref(g_loop);

    g_print("[LOOP] done: captured=%u  zmq_recv=%lu parse_fail=%lu  "
            "detections=%u  pose_join hits=%u misses=%u\n",
            c.captured, rcv, pf, c.dets, c.pose_hits, c.pose_misses);
    return 0;
}
