#include "ddl_bridge.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>

/* src/ headers */
#include "util/event_bus/event_bus.h"
#include "util/event_bus/event_config.h"
#include "app/scheduler/scheduler_events.h"
#include "ddl/ddl_frame.h"
#include "util/log/log.h"
#include "hal/hal.h"
#include "app/app.h"
#include "ddl/servo/servo.h"
#include "ddl/servo/servo_events.h"
#include "ddl/servo/servo_config.h"

/* Orin detection -> lock-on pipeline */
#include "pose_ring.h"
#include "detection_msg.h"
#include "aiming.h"
#include "orin_receiver.h"
#include "tracker.h"

/* PULL-bind endpoint for detections returning from the Orin, which
 * PUSH-connects to it over the direct GigE link. Compile-time: that link is
 * fixed, so this does not vary per deployment. */
#define ORIN_DETECTION_ZMQ_ENDPOINT "tcp://0.0.0.0:5556"

/* Depth of the detection->app forward queue (receiver thread -> main loop). */
#define DET_FWD_QUEUE 16

/* Slew-settling gate: when the commanded pose jumps more than
 * POSE_SETTLE_JUMP_DEG between two consecutive captured frames (a 10-deg scan
 * step or a lock slew), the camera is physically mid-move for a while after,
 * so bearings computed from those frames are garbage. Detections whose capture
 * time falls inside the settle window skip the tracker/aiming (tracks coast
 * through it; deletion only fires on association failure in a tracked frame,
 * so a coasting track that re-matches survives any window length). Lock-follow's
 * small per-detection corrections stay under the threshold and are not gated.
 *
 * The window SCALES WITH THE JUMP: the servo is open-loop with no ramp, so the
 * recorded pose snaps to the end angle while the physical move takes time
 * proportional to its size. A fixed 300 ms covered only the start of a big
 * slew; frames after it were tracked against a pose wrong by several degrees,
 * broke association and churned track ids (the 2026-08-31 lock-loss defect).
 * MS_PER_DEG is a conservative guess for the arm's rate under load — calibrate
 * it from the [TRACKER] spawn/delete log once, then tighten. The cap keeps a
 * 10-deg scan step's window (300 + 500 = 800 ms) inside the 2 s dwell. */
#define POSE_SETTLE_JUMP_DEG   3.0f
#define POSE_SETTLE_BASE_MS    300u
#define POSE_SETTLE_MS_PER_DEG 50u
#define POSE_SETTLE_MAX_MS     1200u

/* Where the optional per-detection log goes (SNIPEIT_LOG_DETECTIONS=1). */
#define DET_LOG_PATH "/tmp/detections.log"

/* Period of the periodic pipeline stats line (0 disables). */
#define BRIDGE_STATS_PERIOD_MS 5000u

struct DdlBridge
{
    WebSocketServer* ws;
    unsigned int     period_ms;
    unsigned long    last_emit_ms;
    bool             hal_up;
    bool             bus_up;
    bool             app_up;
    bool             scheduler_started;
    bool             wind_offset_initialized;
    float            wind_sensor_zero_heading;

    /* Orin detection -> lock-on pipeline. */
    PoseRing*        pose_ring;     /* frame_id -> capture pose join buffer.  */
    OrinReceiver*    orin_rx;       /* ZeroMQ PULL receiver (own thread).     */
    Tracker*         tracker;       /* per-frame indices -> stable track ids. */
    AimConfig        aim_cfg;       /* optics/frame/limits, set once at start.*/
    pthread_mutex_t  lockon_mtx;    /* guards locked / locked_target_id.      */
    bool             lockon_up;     /* lockon_mtx initialised (for teardown). */
    bool             locked;        /* operator has locked on -> auto-follow. */
    bool             has_locked_target_id;
    char             locked_target_id[ORIN_ID_MAXLEN];

    /* Slew-settling gate. last_pose_* are only touched by the capture thread;
     * unsettled_until_ms is written there and read by the Orin receiver
     * thread, so it goes through __atomic ops. */
    bool             have_last_pose;
    float            last_pose_pan, last_pose_tilt;
    uint64_t         unsettled_since_ms;
    uint64_t         unsettled_until_ms;

    /* Pipeline counters (mixed writer threads -> __atomic ops; drained by the
     * stats line on the main thread). */
    unsigned long    n_frames;      /* frames pose-stamped (capture thread).  */
    unsigned long    n_orin_msgs;   /* detection msgs received (rx thread).   */
    unsigned long    n_pose_miss;   /* msgs whose frame_id->pose join missed. */
    unsigned long    n_slew_skip;   /* msgs gated by the settling window.     */
    unsigned long    n_aim_updates; /* servo targets pushed from detections.  */

    /* Detection -> app forward queue. The Orin receiver thread serialises each
     * detection message here; the main loop (ddl_bridge_pump_detections) drains
     * it and ws_send_json's, keeping all WebSocket sends on the main thread. */
    pthread_mutex_t  det_mtx;
    bool             det_up;
    int              det_head, det_tail, det_count;
    char             det_queue[DET_FWD_QUEUE][WS_MAX_MSG_SIZE];

    /* Optional detection log, enabled per run with SNIPEIT_LOG_DETECTIONS=1.
     * Its own file rather than stdout so the operational log stays readable and
     * the detection stream can be grepped or diffed across runs. Opened in
     * ddl_bridge_start, written only by the main loop in
     * ddl_bridge_pump_detections, closed in ddl_bridge_stop. */
    FILE*            det_log;
};

static unsigned long long now_ms_mono64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000L);
}

/* Choose which detection to follow, keyed on the STABLE track id (ids[i]):
 * the locked id if one was supplied, else the highest-confidence detection.
 * Only CONFIRMED tracks (confirmed[i]) are lockable, so a one-frame false
 * positive can't grab the servos. Returns an index or -1. */
static int select_detection_index(const OrinDetectionMsg* msg,
                                  bool has_id, const char* id,
                                  const uint32_t* ids, const bool* confirmed)
{
    int   best      = -1;
    float best_conf = -1.0f;
    for (int i = 0; i < msg->num_detections; i++)
    {
        if (!confirmed[i])
        {
            continue;   /* tentative track: not lockable yet */
        }
        if (has_id)
        {
            char idbuf[ORIN_ID_MAXLEN];
            snprintf(idbuf, sizeof(idbuf), "%u", ids[i]);
            if (strcmp(idbuf, id) == 0)
            {
                return i;   /* stable id match wins immediately */
            }
        }
        else if (msg->detections[i].confidence > best_conf)
        {
            best_conf = msg->detections[i].confidence;
            best      = i;
        }
    }
    return has_id ? -1 : best;
}

/* Serialise a parsed Orin detection message into the EXACT "target_detection"
 * schema the app already consumes (compact, bbox in 1920x1080) — the same wire
 * shape the retired Python detector produced. The base schema is unchanged;
 * one OPTIONAL field is added (see below).
 *
 * When @ids is non-NULL the "id" field carries the STABLE track id ids[i]
 * (instead of the Orin's per-frame index), and an OPTIONAL non-breaking
 * "confirmed" bool is added from confirmed[i] (the app may ignore it, or dim
 * tentative tracks / withhold the lock affordance). When @ids is NULL (a pose
 * miss, no tracking this frame) the Orin's per-frame id is passed through and
 * no "confirmed" field is emitted. Returns bytes written, or -1 on overflow. */
static int build_app_detection_json(const OrinDetectionMsg* m,
                                    const uint32_t* ids, const bool* confirmed,
                                    char* out, size_t cap)
{
    int n = snprintf(out, cap,
        "{\"type\":\"target_detection\",\"timestamp_ms\":%llu,\"detections\":[",
        (unsigned long long)m->timestamp_ms);
    if (n < 0 || (size_t)n >= cap) return -1;

    for (int i = 0; i < m->num_detections; i++)
    {
        const OrinDetection* d = &m->detections[i];

        char idbuf[ORIN_ID_MAXLEN];
        const char* idstr = d->target_id;
        if (ids != NULL)
        {
            snprintf(idbuf, sizeof(idbuf), "%u", ids[i]);
            idstr = idbuf;
        }

        char cfield[24];
        cfield[0] = '\0';
        if (confirmed != NULL)
        {
            snprintf(cfield, sizeof(cfield), ",\"confirmed\":%s",
                     confirmed[i] ? "true" : "false");
        }

        int k = snprintf(out + n, cap - (size_t)n,
            "%s{\"id\":\"%s\",\"class\":\"%s\",\"confidence\":%.2f,"
            "\"bbox\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}%s}",
            i ? "," : "", idstr, d->cls, (double)d->confidence,
            d->bbox_x, d->bbox_y, d->bbox_w, d->bbox_h, cfield);
        if (k < 0 || (size_t)(n + k) >= cap) return -1;
        n += k;
    }
    int k = snprintf(out + n, cap - (size_t)n, "]}");
    if (k < 0 || (size_t)(n + k) >= cap) return -1;
    return n + k;
}

/* Push a serialised detection line onto the receiver->main forward queue.
 * Runs on the receiver thread; drops the oldest entry if the queue is full so
 * the app always gets the freshest boxes. */
static void det_enqueue(DdlBridge* b, const char* json, size_t len)
{
    if (!b->det_up || len == 0 || len >= WS_MAX_MSG_SIZE) return;

    pthread_mutex_lock(&b->det_mtx);
    if (b->det_count == DET_FWD_QUEUE)
    {
        b->det_tail = (b->det_tail + 1) % DET_FWD_QUEUE;
        b->det_count--;
    }
    memcpy(b->det_queue[b->det_head], json, len);
    b->det_queue[b->det_head][len] = '\0';
    b->det_head = (b->det_head + 1) % DET_FWD_QUEUE;
    b->det_count++;
    pthread_mutex_unlock(&b->det_mtx);
}

/* Runs on the Orin receiver thread (see orin_receiver.h threading contract).
 * Forwards every detection message to the app (unconditionally, for the box
 * overlay), then drives the servos toward the locked target when locked. Only
 * ddl_servo_set_target is touched for the servos (mutex-protected); the servo
 * FSM is already in TARGET_LOCK and re-reads the target on its periodic
 * DIRECTIONS tick, so updating the target makes it slew to follow. */
static void orin_detection_handler(const OrinDetectionMsg* msg,
                                   const PoseEntry* pose, void* user)
{
    DdlBridge* b = (DdlBridge*)user;

    /* Run the tracker to turn the Orin's per-frame indices into STABLE ids —
     * but only when the pose join HIT (the tracker maps each bbox to a world
     * bearing using the pose). On a MISS we forward per-frame ids and do no
     * tracking or servo work this frame (tracks coast; the sender tags every
     * frame so misses are rare). */
    uint32_t ids[ORIN_MAX_DETECTIONS];
    bool     confirmed[ORIN_MAX_DETECTIONS];
    bool     tracked = (pose != NULL && b->tracker != NULL);

    __atomic_fetch_add(&b->n_orin_msgs, 1UL, __ATOMIC_RELAXED);
    if (pose == NULL)
    {
        __atomic_fetch_add(&b->n_pose_miss, 1UL, __ATOMIC_RELAXED);
    }

    /* Slew-settling gate: if this frame was captured inside the settle window
     * after a big commanded-pose jump, its recorded pose is the jump's END
     * angles while the camera was physically mid-move. Treat it like a pose
     * miss: forward the raw boxes, no tracker/servo work, tracks coast. */
    if (tracked)
    {
        /* since/until are written separately on the capture thread; the pair
         * is not read atomically, so a racing window update can mis-gate at
         * most this one frame — same tolerance as the counters here. */
        uint64_t until = __atomic_load_n(&b->unsettled_until_ms, __ATOMIC_RELAXED);
        uint64_t since = __atomic_load_n(&b->unsettled_since_ms, __ATOMIC_RELAXED);
        if (pose->capture_ts_ms >= since && pose->capture_ts_ms < until)
        {
            tracked = false;
            __atomic_fetch_add(&b->n_slew_skip, 1UL, __ATOMIC_RELAXED);
        }
    }

    if (tracked)
    {
        tracker_update(b->tracker, msg, pose, ids, confirmed);
    }

    /* App overlay feed: forward EVERY message (incl. empty detections, which
     * clear the app's boxes) regardless of lock state. Stable ids + a
     * non-breaking "confirmed" flag when tracked; per-frame ids on a miss.
     * Sent on the main thread by ddl_bridge_pump_detections. */
    char json[WS_MAX_MSG_SIZE];
    int jn = build_app_detection_json(msg, tracked ? ids : NULL,
                                      tracked ? confirmed : NULL,
                                      json, sizeof(json));
    if (jn > 0)
    {
        det_enqueue(b, json, (size_t)jn);
    }

    pthread_mutex_lock(&b->lockon_mtx);
    bool locked = b->locked;
    bool has_id = b->has_locked_target_id;
    char id[ORIN_ID_MAXLEN];
    memcpy(id, b->locked_target_id, sizeof(id));
    pthread_mutex_unlock(&b->lockon_mtx);

    /* Servo lock-follow needs: locked, a non-empty frame, and a pose (tracked).
     * On a miss we skip — aiming a stale bbox against the wrong pose can nudge
     * the servo the wrong way. */
    if (!locked || !tracked || msg->num_detections == 0)
    {
        return;
    }

    int idx = select_detection_index(msg, has_id, id, ids, confirmed);
    if (idx < 0)
    {
        return;   /* locked (confirmed) target not present in this frame */
    }
    const OrinDetection* det = &msg->detections[idx];

    float pan  = pose->hor_angle;
    float tilt = pose->ver_angle;

    AimSolution sol;
    if (!aim_compute(&b->aim_cfg, det->bbox_x, det->bbox_y,
                     det->bbox_w, det->bbox_h, pan, tilt, &sol))
    {
        return;
    }

    /* aim_compute already clamped to the servo travel limits, so set_target
     * won't reject the angles. TARGET_UPDATE makes the lock state apply it
     * NOW: without it the target is only sampled on the scheduler's 2 s
     * DIRECTIONS tick, so follow moved in one big jump per 2 s. */
    if (ddl_servo_set_target(sol.pan_deg, sol.tilt_deg) == eSTATUS_SUCCESSFUL)
    {
        __atomic_fetch_add(&b->n_aim_updates, 1UL, __ATOMIC_RELAXED);
        (void)util_event_bus_publish(eAO_SERVO, eSERVO_EVENT_TARGET_UPDATE);
    }
}

/* Monotonic clock — used for the period gate (immune to wall-clock changes). */
static unsigned long now_ms_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL +
           (unsigned long)(ts.tv_nsec / 1000000L);
}

/* Wall-clock ms since epoch — goes into the JSON "timestamp" field so the
 * app can correlate with its own clock. */
static unsigned long long now_ms_epoch(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000L);
}

static int build_json(const DDLFrame* f, const DdlBridge* b, char* out, size_t cap)
{
    const DistanceFrame*            d  = &f->dist_frame;
    const TemperatureHumidityFrame* th = &f->temp_hum_frame;
    const ServoFrame*               s  = &f->servo_frame;
    const GPSFrame*                 g  = &f->gps_frame;
    const MagFrame*                 c  = &f->mag_frame;
    const WindFrame*                w  = &f->wind_frame;

    float wind_direction_deg = 0.0f;
    if(b->wind_offset_initialized)
    {
        wind_direction_deg = fmodf(b->wind_sensor_zero_heading + w->direction_degrees, 360.0f);
        if(wind_direction_deg < 0.0f)
        {
            wind_direction_deg += 360.0f;
        }
    }
    else
    {
        wind_direction_deg = w->direction_degrees;
    }

    char heading_buf[32];
    if (c->valid && isfinite(c->heading_deg))
    {
        snprintf(heading_buf, sizeof(heading_buf), "%.2f", (double)c->heading_deg);
    }
    else
    {
        snprintf(heading_buf, sizeof(heading_buf), "null");
    }

    return snprintf(out, cap,
        "{"
            "\"type\":\"sensor_data\","
            "\"timestamp\":%llu,"
            "\"ddl_frame\":{"
                "\"distance\":{"
                    "\"valid\":%s,"
                    "\"distance_m\":%.2f,"
                    "\"status\":%u,"
                    "\"precision\":%u,"
                    "\"strength\":%u"
                "},"
                "\"temperature_humidity\":{"
                    "\"valid\":%s,"
                    "\"temperature_c\":%.2f,"
                    "\"humidity_pct\":%.2f"
                "},"
                "\"servo\":{"
                    "\"horizontal_deg\":%.2f,"
                    "\"vertical_deg\":%.2f"
                "},"
                "\"gps\":{"
                    "\"valid\":%s,"
                    "\"fix_type\":%u,"
                    "\"num_satellites\":%u,"
                    "\"latitude_deg\":%.7f,"
                    "\"longitude_deg\":%.7f,"
                    "\"altitude_m\":%.2f,"
                    "\"h_acc_m\":%.2f"
                "},"
                "\"compass\":{"
                    "\"valid\":%s,"
                    "\"raw_x\":%u,"
                    "\"raw_y\":%u,"
                    "\"raw_z\":%u,"
                    "\"temperature_c\":%.2f,"
                    "\"heading_deg\":%s"
                "},"
                "\"wind\":{"
                    "\"speed_valid\":%s,"
                    "\"speed_mps\":%.2f,"
                    "\"direction_valid\":%s,"
                    "\"direction_deg\":%.2f"
                "}"
            "}"
        "}",
        now_ms_epoch(),
        d->valid ? "true" : "false",
            (double)d->distance,
            (unsigned)d->status, (unsigned)d->precision, (unsigned)d->strength,
        th->valid ? "true" : "false",
            (double)th->temperature, (double)th->humidity,
        (double)s->hor_angle, (double)s->ver_angle,
        g->valid ? "true" : "false",
            (unsigned)g->fix_type, (unsigned)g->num_satellites,
            g->latitude, g->longitude,
            (double)g->altitude, (double)g->h_acc,
        c->valid ? "true" : "false",
            (unsigned)c->raw_x, (unsigned)c->raw_y, (unsigned)c->raw_z,
            (double)c->temperature_c, heading_buf,
        w->speed_valid ? "true" : "false", (double)w->speed,
        w->direction_valid ? "true" : "false", (double)wind_direction_deg);
}

static void calibrate_wind_offset(DdlBridge* b, const DDLFrame* snap)
{
    if(b->wind_offset_initialized)
    {
        return;
    }

    const MagFrame* c = &snap->mag_frame;
    const ServoFrame* s = &snap->servo_frame;

    if(!c->valid || !isfinite(c->heading_deg))
    {
        return;
    }

    /* Reject the reading unless the arm is at the calibration position.
     * Servos are open-loop on this rig, so hor_angle reflects the last
     * commanded angle; a tight tolerance is fine. */
    if(fabsf(s->hor_angle - 90.0f) > 2.0f)
    {
        return;
    }

    b->wind_sensor_zero_heading = c->heading_deg;
    b->wind_offset_initialized  = true;

    printf("[BRIDGE] Wind sensor 0-mark heading calibrated to %.2f deg "
           "(servo hor_angle %.2f)\n",
           (double)b->wind_sensor_zero_heading,
           (double)s->hor_angle);
}

DdlBridge* ddl_bridge_start(WebSocketServer* ws, unsigned int period_ms)
{
    if(ws == NULL)
    {
        return NULL;
    }

    DdlBridge* b = calloc(1, sizeof(*b));
    if(b == NULL)
    {
        return NULL;
    }
    b->ws = ws;
    b->period_ms = period_ms;

    if(log_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] log_init failed\n");
        free(b);
        return NULL;
    }

    if(hal_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] hal_init failed\n");
        log_exit();
        free(b);
        return NULL;
    }
    b->hal_up = true;

    if(util_event_bus_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] event bus init failed\n");
        ddl_bridge_stop(b);
        return NULL;
    }
    b->bus_up = true;

    if(app_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] app_init failed\n");
        ddl_bridge_stop(b);
        return NULL;
    }
    b->app_up = true;

    if(util_event_bus_publish(eAO_SCHEDULER, eSCHEDULER_EVENT_START) != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] failed to publish scheduler start\n");
        ddl_bridge_stop(b);
        return NULL;
    }
    b->scheduler_started = true;

    /* Orin detection -> lock-on pipeline. NON-FATAL: if the pose ring or the
     * ZeroMQ bind fails we still serve sensors/streaming, just without
     * auto lock-on (the manual app commands keep working). */
    aim_config_default(&b->aim_cfg);
    /* Tracker borrows aim_cfg (a stable struct field) for its bbox->bearing
     * geometry. NON-FATAL: without it, detections still forward with per-frame
     * ids, just no stable-id lock-by-id. */
    b->tracker = tracker_create(&b->aim_cfg);
    if (b->tracker == NULL)
    {
        fprintf(stderr, "[BRIDGE] WARNING: tracker alloc failed; "
                        "stable track ids disabled\n");
    }
    if (pthread_mutex_init(&b->det_mtx, NULL) == 0)
    {
        b->det_up = true;   /* detection->app forward queue ready */
    }

    if (getenv("SNIPEIT_LOG_DETECTIONS") != NULL)
    {
        b->det_log = fopen(DET_LOG_PATH, "w");
        if (b->det_log == NULL)
        {
            fprintf(stderr, "[BRIDGE] WARNING: could not open %s: detection "
                            "logging disabled\n", DET_LOG_PATH);
        }
        else
        {
            setvbuf(b->det_log, NULL, _IOLBF, 0);   /* line-buffered: tail -f works */
            printf("[BRIDGE] Detection logging enabled -> %s\n", DET_LOG_PATH);
        }
    }
    if (pthread_mutex_init(&b->lockon_mtx, NULL) == 0)
    {
        b->lockon_up  = true;
        b->pose_ring  = pose_ring_create(POSE_RING_DEFAULT_CAPACITY);
        if (b->pose_ring != NULL)
        {
            b->orin_rx = orin_receiver_start(ORIN_DETECTION_ZMQ_ENDPOINT,
                                             b->pose_ring,
                                             orin_detection_handler, b);
            if (b->orin_rx == NULL)
            {
                fprintf(stderr, "[BRIDGE] WARNING: Orin detection receiver "
                                "failed to start; auto lock-on disabled\n");
            }
        }
        else
        {
            fprintf(stderr, "[BRIDGE] WARNING: pose ring alloc failed; "
                            "auto lock-on disabled\n");
        }
    }
    else
    {
        fprintf(stderr, "[BRIDGE] WARNING: lock-on mutex init failed; "
                        "auto lock-on disabled\n");
    }

    printf("[BRIDGE] Started — emitting sensor_data every %u ms\n", period_ms);
    return b;
}

void ddl_bridge_tick(DdlBridge* b)
{
    if(b == NULL || !b->scheduler_started)
    {
        return;
    }

    if(!ws_is_client_connected(b->ws))
    {
        return;
    }

    unsigned long t = now_ms_mono();
    if(b->last_emit_ms != 0 && (t - b->last_emit_ms) < b->period_ms)
    {
        return;
    }
    b->last_emit_ms = t;

    const DDLFrame* snap = app_get_ddl_snapshot();
    if(snap == NULL)
    {
        return;
    }

    calibrate_wind_offset(b, snap);

    char buf[1024];
    int n = build_json(snap, b, buf, sizeof(buf));
    if(n <= 0 || (size_t)n >= sizeof(buf))
    {
        fprintf(stderr, "[BRIDGE] JSON build failed (n=%d)\n", n);
        return;
    }

    if(ws_send_json(b->ws, buf, (size_t)n) != 0)
    {
        fprintf(stderr, "[BRIDGE] ws_send_json failed (queue full?)\n");
    }
}

void ddl_bridge_stop(DdlBridge* b)
{
    if(b == NULL)
    {
        return;
    }

    /* Stop the Orin receiver FIRST so no detection callback touches the servo
     * or the DDL snapshot while we tear the app down below. */
    if(b->orin_rx)
    {
        orin_receiver_stop(b->orin_rx);
        b->orin_rx = NULL;
    }
    /* Receiver stopped -> no handler is running -> safe to free the tracker. */
    if(b->tracker)
    {
        tracker_destroy(b->tracker);
        b->tracker = NULL;
    }
    if(b->pose_ring)
    {
        pose_ring_destroy(b->pose_ring);
        b->pose_ring = NULL;
    }
    if(b->lockon_up)
    {
        pthread_mutex_destroy(&b->lockon_mtx);
        b->lockon_up = false;
    }
    if(b->det_log != NULL)
    {
        fclose(b->det_log);
        b->det_log = NULL;
    }

    if(b->det_up)
    {
        pthread_mutex_destroy(&b->det_mtx);
        b->det_up = false;
    }

    /* Tear down in reverse order, only what we actually brought up. */
    if(b->app_up)
    {
        (void)app_end();
        app_join();
        app_delete();
    }
    if(b->bus_up)
    {
        util_event_bus_delete();
    }
    if(b->hal_up)
    {
        hal_cleanup();
    }
    log_exit();
    free(b);
}

/* Minimal flat-JSON extractors — fine for our own well-formed command frames. */
static bool json_get_string(const char* json, const char* key, char* out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
    return (*p == '"');
}

static bool json_get_number(const char* json, const char* key, double* out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    return sscanf(p + 1, " %lf", out) == 1;
}

void ddl_bridge_handle_command(DdlBridge* b, const char* json, size_t len)
{
    (void)len;
    if (json == NULL) return;

    char command[32];
    if (!json_get_string(json, "command", command, sizeof(command)))
        return;   /* not a command frame (e.g. the keepalive ping) — ignore */

    if (strcmp(command, "set_servo_angles") == 0)
    {
        double h = 0.0, v = 0.0;
        if (!json_get_number(json, "horizontal_deg", &h) ||
            !json_get_number(json, "vertical_deg",   &v))
            return;

        /* Clamp to the servo's mechanical pan/tilt range so set_target
         * isn't rejected at the extremes (the app clamps too; this is the
         * backstop). */
        if (h < SERVO_HORIZONTAL_MIN_ANGLE_DEG) h = SERVO_HORIZONTAL_MIN_ANGLE_DEG;
        if (h > SERVO_HORIZONTAL_MAX_ANGLE_DEG) h = SERVO_HORIZONTAL_MAX_ANGLE_DEG;
        if (v < SERVO_VERTICAL_MIN_ANGLE_DEG)   v = SERVO_VERTICAL_MIN_ANGLE_DEG;
        if (v > SERVO_VERTICAL_MAX_ANGLE_DEG)   v = SERVO_VERTICAL_MAX_ANGLE_DEG;

        if (ddl_servo_set_target((float)h, (float)v) != eSTATUS_SUCCESSFUL)
            return;
        /* Manual aim overrides any auto-follow: stop tracking detections. */
        if (b->lockon_up)
        {
            pthread_mutex_lock(&b->lockon_mtx);
            b->locked = false;
            b->has_locked_target_id = false;
            pthread_mutex_unlock(&b->lockon_mtx);
        }
        (void)util_event_bus_publish(eAO_SERVO, eSERVO_EVENT_NOISE_DETECTED);
        printf("[CMD] Slew to servo (%.1f, %.1f) + scan\n", h, v);
    }
    else if (strcmp(command, "select_target") == 0)
    {
        char action[16] = {0};
        (void)json_get_string(json, "action", action, sizeof(action));
        if (strcmp(action, "lock") == 0)
        {
            /* Optional target id: follow only this id if given, else the
             * highest-confidence detection. App may send "target_id" or "id". */
            char tid[ORIN_ID_MAXLEN] = {0};
            bool has_tid = json_get_string(json, "target_id", tid, sizeof(tid)) ||
                           json_get_string(json, "id", tid, sizeof(tid));
            if (b->lockon_up)
            {
                pthread_mutex_lock(&b->lockon_mtx);
                b->locked               = true;
                b->has_locked_target_id = has_tid;
                if (has_tid)
                {
                    strncpy(b->locked_target_id, tid,
                            sizeof(b->locked_target_id) - 1);
                    b->locked_target_id[sizeof(b->locked_target_id) - 1] = '\0';
                }
                pthread_mutex_unlock(&b->lockon_mtx);
            }
            (void)util_event_bus_publish(eAO_SERVO, eSERVO_EVENT_LOCK);
            printf("[CMD] Lock on%s%s\n", has_tid ? " target " : "",
                   has_tid ? tid : " (best detection)");
        }
        else if (strcmp(action, "unlock") == 0)
        {
            if (b->lockon_up)
            {
                pthread_mutex_lock(&b->lockon_mtx);
                b->locked               = false;
                b->has_locked_target_id = false;
                pthread_mutex_unlock(&b->lockon_mtx);
            }
            (void)util_event_bus_publish(eAO_SERVO, eSERVO_EVENT_SCAN);
            printf("[CMD] Unlock -> scan\n");
        }
    }
    /* anything else (incl. "ping"): ignore */
}

/* Periodic one-line pipeline health report (rates over the last period).
 * cap = frames reaching the Orin encoder per second (post-drop, so below raw
 * capture fps when the encoder is behind), orin = detection
 * msgs/s back from the Orin, miss = pose-join misses, slew_skip = msgs
 * gated by the settling window, aim = servo follow updates pushed. */
static void bridge_stats_tick(DdlBridge* b)
{
    /* Reached only from ddl_bridge_pump_detections, which runs on the main
     * loop (main.c), so these statics need no synchronisation. */
    /* cppcheck-suppress threadsafety-threadsafety */
    static uint64_t      last_ms;
    /* cppcheck-suppress threadsafety-threadsafety */
    static unsigned long last_frames, last_msgs, last_miss, last_skip, last_aim;

    uint64_t now = now_ms_mono64();
    if (last_ms == 0)
    {
        last_ms = now;
        return;
    }
    if (now - last_ms < BRIDGE_STATS_PERIOD_MS)
    {
        return;
    }

    unsigned long f  = __atomic_load_n(&b->n_frames,      __ATOMIC_RELAXED);
    unsigned long m  = __atomic_load_n(&b->n_orin_msgs,   __ATOMIC_RELAXED);
    unsigned long pm = __atomic_load_n(&b->n_pose_miss,   __ATOMIC_RELAXED);
    unsigned long sk = __atomic_load_n(&b->n_slew_skip,   __ATOMIC_RELAXED);
    unsigned long au = __atomic_load_n(&b->n_aim_updates, __ATOMIC_RELAXED);

    float dt_s = (float)(now - last_ms) / 1000.0f;
    printf("[BRIDGE] stats: cap=%.1f fps  orin=%.1f msg/s  miss=%lu  "
           "slew_skip=%lu  aim=%lu  (last %.0fs)\n",
           (float)(f - last_frames) / dt_s,
           (float)(m - last_msgs)   / dt_s,
           pm - last_miss, sk - last_skip, au - last_aim, (double)dt_s);

    last_ms     = now;
    last_frames = f;
    last_msgs   = m;
    last_miss   = pm;
    last_skip   = sk;
    last_aim    = au;
}

void ddl_bridge_pump_detections(DdlBridge* b)
{
    if (b == NULL || !b->det_up)
    {
        return;
    }

    bridge_stats_tick(b);

    for (;;)
    {
        char line[WS_MAX_MSG_SIZE];
        size_t len;

        pthread_mutex_lock(&b->det_mtx);
        if (b->det_count == 0)
        {
            pthread_mutex_unlock(&b->det_mtx);
            break;
        }
        len = strlen(b->det_queue[b->det_tail]);
        memcpy(line, b->det_queue[b->det_tail], len + 1);
        b->det_tail = (b->det_tail + 1) % DET_FWD_QUEUE;
        b->det_count--;
        pthread_mutex_unlock(&b->det_mtx);

        (void)ws_send_json(b->ws, line, len);

        /* Off unless SNIPEIT_LOG_DETECTIONS was set for this run. The JSON
         * already carries the stable track id and the confirmed flag, so the
         * line is the full record of what the app was told. */
        if (b->det_log != NULL)
        {
            fprintf(b->det_log, "%llu %s\n", now_ms_mono64(), line);
        }
    }
}

void ddl_bridge_record_capture_pose(DdlBridge* b, uint32_t frame_id)
{
    if (b == NULL || b->pose_ring == NULL)
    {
        return;
    }

    /* Live commanded angles, NOT the broadcaster snapshot. The snapshot only
     * refreshes once per 2 s scheduler cycle, so it stamps frames with the
     * pre-step pose after a scan step — ~10-deg bearing error, full track
     * churn. The snapshot is the fallback only. */
    float pan, tilt;
    if (ddl_servo_get_pose(&pan, &tilt) != eSTATUS_SUCCESSFUL)
    {
        const DDLFrame* snap = app_get_ddl_snapshot();
        if (snap == NULL)
        {
            return;
        }
        pan  = snap->servo_frame.hor_angle;
        tilt = snap->servo_frame.ver_angle;
    }

    uint64_t now = now_ms_mono64();

    /* Open (or extend) the slew-settling window when the pose jumps between
     * frames, sized by the larger axis jump — see the macro comment. A jump
     * landing inside an open window keeps the window's start and only pushes
     * its end, so back-to-back corrections read as one disturbance. */
    if (b->have_last_pose)
    {
        float jump = fmaxf(fabsf(pan  - b->last_pose_pan),
                           fabsf(tilt - b->last_pose_tilt));
        if (jump > POSE_SETTLE_JUMP_DEG)
        {
            uint64_t win = POSE_SETTLE_BASE_MS +
                           (uint64_t)(jump * (float)POSE_SETTLE_MS_PER_DEG);
            if (win > POSE_SETTLE_MAX_MS)
            {
                win = POSE_SETTLE_MAX_MS;
            }
            uint64_t until = __atomic_load_n(&b->unsettled_until_ms,
                                             __ATOMIC_RELAXED);
            if (now >= until)   /* window closed: this jump opens a fresh one */
            {
                __atomic_store_n(&b->unsettled_since_ms, now, __ATOMIC_RELAXED);
            }
            if (now + win > until)
            {
                __atomic_store_n(&b->unsettled_until_ms, now + win,
                                 __ATOMIC_RELAXED);
            }
        }
    }
    b->last_pose_pan  = pan;
    b->last_pose_tilt = tilt;
    b->have_last_pose = true;

    __atomic_fetch_add(&b->n_frames, 1UL, __ATOMIC_RELAXED);
    pose_ring_record(b->pose_ring, frame_id, pan, tilt, now);
}