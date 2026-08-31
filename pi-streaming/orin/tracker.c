/**
 * tracker.c
 *
 * Greedy nearest-neighbour tracker in motion-compensated angular space.
 * See tracker.h for the design rationale.
 */

#include "tracker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Tunables ------------------------------------------------------------ */
#define GATE_MIN_DEG   2.0f    /* minimum association gate (degrees)           */
#define GATE_K         1.5f    /* gate also scales with target angular width   */
#define MAX_COAST_MS   1500u   /* delete a track unseen for this long          */
#define N_INIT         2       /* hits before a track is "confirmed"           */
#define BEARING_ALPHA  0.5f    /* EMA smoothing of the track bearing           */
#define PIN_GATE_MULT  2.0f    /* gate widening for the pinned (locked) track  */

#define DEG2RAD (0.017453292519943295f)
#define RAD2DEG (57.29577951308232f)

typedef struct
{
    bool     used;
    uint32_t id;
    char     cls[ORIN_CLASS_MAXLEN];
    float    pan;            /* smoothed world bearing, degrees                */
    float    tilt;
    /* EXTENSION POINT: to lead fast slews, add `float vpan, vtilt` (deg/s from
     * (new-old)/dt) and predict `pan += vpan*dt` before gating. Local change,
     * not a rewrite. Only worth it if measurement shows a target crossing more
     * than the gate between two consecutive detections - at the ~2.4-3.4 fps
     * the Orin round trip actually delivers, that is plausible during a hard
     * slew, so measure before assuming plain greedy-NN is enough. */
    float    ang_w;          /* last angular width, degrees (for gating)       */
    int      hits;
    uint64_t last_seen_ms;
} Track;

struct Tracker
{
    const AimConfig *aim;
    Track            tracks[TRACKER_MAX_TRACKS];
    uint32_t         next_id;   /* stable id counter; 0 is reserved "none"     */
    uint32_t         pinned_id; /* locked track id, 0 = none; set from other
                                   threads, so __atomic access only            */
};

Tracker *tracker_create(const AimConfig *aim)
{
    if (aim == NULL)
    {
        return NULL;
    }
    Tracker *t = calloc(1, sizeof(*t));
    if (t == NULL)
    {
        return NULL;
    }
    t->aim     = aim;
    t->next_id = 1;
    return t;
}

void tracker_destroy(Tracker *t)
{
    free(t);
}

void tracker_set_pinned_id(Tracker *t, uint32_t id)
{
    if (t == NULL)
    {
        return;
    }
    __atomic_store_n(&t->pinned_id, id, __ATOMIC_RELAXED);
}

/* Map a detection's bbox + capture pose to its absolute world bearing and its
 * angular width (used to scale the gate). Reuses the aiming geometry: the
 * unclamped absolute angle is capture_pose + the (signed) aim offset. */
static void det_bearing(const Tracker *t, const OrinDetection *d,
                        const PoseEntry *pose,
                        float *abs_pan, float *abs_tilt, float *ang_w)
{
    AimSolution sol;
    if (aim_compute(t->aim, d->bbox_x, d->bbox_y, d->bbox_w, d->bbox_h,
                    pose->hor_angle, pose->ver_angle, &sol))
    {
        *abs_pan  = pose->hor_angle + sol.pan_offset_deg;
        *abs_tilt = pose->ver_angle + sol.tilt_offset_deg;
    }
    else
    {
        *abs_pan  = pose->hor_angle;
        *abs_tilt = pose->ver_angle;
    }

    float half_hfov = t->aim->hfov_deg * 0.5f * DEG2RAD;
    float frac      = (t->aim->frame_w > 0)
                        ? (float)d->bbox_w / (float)t->aim->frame_w : 0.0f;
    *ang_w = 2.0f * atanf(frac * tanf(half_hfov)) * RAD2DEG;
}

static int find_free_or_evict(Tracker *t, uint32_t pinned)
{
    for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
    {
        if (!t->tracks[k].used)
        {
            return k;
        }
    }
    /* Full: evict the track unseen the longest; the pinned track is exempt. */
    int      slot   = -1;
    uint64_t oldest = 0;
    for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
    {
        if (t->tracks[k].id == pinned && pinned != 0)
        {
            continue;
        }
        if (slot < 0 || t->tracks[k].last_seen_ms < oldest)
        {
            oldest = t->tracks[k].last_seen_ms;
            slot   = k;
        }
    }
    printf("[TRACKER] evict id=%u cls=%s hits=%d (slots full)\n",
           t->tracks[slot].id, t->tracks[slot].cls, t->tracks[slot].hits);
    return slot;
}

/* Fold detection i into track tr: EMA the bearing, refresh the lifecycle. */
static void bind_detection(Track *tr, int i, const float *dp, const float *dt,
                           const float *dw, uint64_t now,
                           uint32_t *out_ids, bool *out_confirmed)
{
    tr->pan  += BEARING_ALPHA * (dp[i] - tr->pan);
    tr->tilt += BEARING_ALPHA * (dt[i] - tr->tilt);
    tr->ang_w        = dw[i];
    tr->hits        += 1;
    tr->last_seen_ms = now;
    out_ids[i]       = tr->id;
    out_confirmed[i] = (tr->hits >= N_INIT);
}

void tracker_update(Tracker *t, const OrinDetectionMsg *msg, const PoseEntry *pose,
                    uint32_t *out_ids, bool *out_confirmed)
{
    if (t == NULL || msg == NULL || pose == NULL)
    {
        return;
    }

    /* Time base is the frame's CAPTURE timestamp, not wall clock, so coasting
     * is measured in capture time. ZeroMQ PUSH/PULL over TCP delivers in
     * capture order; the coast check at the bottom does not rely on that. */
    const uint64_t now = pose->capture_ts_ms;
    const int      nd  = (msg->num_detections < ORIN_MAX_DETECTIONS)
                            ? msg->num_detections : ORIN_MAX_DETECTIONS;

    /* Per-detection bearing, elevation and angular width. Only [0, nd) is used. */
    float dp[ORIN_MAX_DETECTIONS] = {0};
    float dt[ORIN_MAX_DETECTIONS] = {0};
    float dw[ORIN_MAX_DETECTIONS] = {0};
    bool  det_taken[ORIN_MAX_DETECTIONS];
    bool  trk_matched[TRACKER_MAX_TRACKS];

    for (int i = 0; i < nd; i++)
    {
        det_bearing(t, &msg->detections[i], pose, &dp[i], &dt[i], &dw[i]);
        det_taken[i]     = false;
        out_ids[i]       = 0;
        out_confirmed[i] = false;
    }
    for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
    {
        trk_matched[k] = false;
    }

    /* Loaded once per frame: a pin racing this frame applies from the next. */
    const uint32_t pinned = __atomic_load_n(&t->pinned_id, __ATOMIC_RELAXED);

    /* The pinned track matches first, with a widened gate: the operator's lock
     * must not lose its detection to a nearby track, and must re-acquire
     * through bearing noise a normal track would reject. */
    if (pinned != 0)
    {
        for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
        {
            Track *tr = &t->tracks[k];
            if (!tr->used || tr->id != pinned)
            {
                continue;
            }
            float best = 1e9f;
            int   bi   = -1;
            for (int i = 0; i < nd; i++)
            {
                if (strcmp(tr->cls, msg->detections[i].cls) != 0) continue;
                float gate = PIN_GATE_MULT * fmaxf(GATE_MIN_DEG, GATE_K * dw[i]);
                float dist = hypotf(dp[i] - tr->pan, dt[i] - tr->tilt);
                if (dist <= gate && dist < best)
                {
                    best = dist; bi = i;
                }
            }
            if (bi >= 0)
            {
                bind_detection(tr, bi, dp, dt, dw, now, out_ids, out_confirmed);
                det_taken[bi]  = true;
                trk_matched[k] = true;
            }
            break;
        }
    }

    /* Greedy: repeatedly bind the globally smallest un-gated (det,track) pair,
     * same class only. N is tiny so O(N^2) per pass is fine. */
    for (;;)
    {
        float best = 1e9f;
        int   bi = -1, bk = -1;

        for (int i = 0; i < nd; i++)
        {
            if (det_taken[i]) continue;
            float gate = fmaxf(GATE_MIN_DEG, GATE_K * dw[i]);
            for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
            {
                Track *tr = &t->tracks[k];
                if (!tr->used || trk_matched[k]) continue;
                if (strcmp(tr->cls, msg->detections[i].cls) != 0) continue;
                float dpan = dp[i] - tr->pan;
                float dtil = dt[i] - tr->tilt;
                float dist = hypotf(dpan, dtil);
                if (dist <= gate && dist < best)
                {
                    best = dist; bi = i; bk = k;
                }
            }
        }
        if (bi < 0 || bk < 0) break;

        bind_detection(&t->tracks[bk], bi, dp, dt, dw, now,
                       out_ids, out_confirmed);
        det_taken[bi]   = true;
        trk_matched[bk] = true;
    }

    /* Spawn a new track for every unmatched detection. */
    for (int i = 0; i < nd; i++)
    {
        if (det_taken[i]) continue;
        int slot = find_free_or_evict(t, pinned);
        Track *tr = &t->tracks[slot];
        memset(tr, 0, sizeof(*tr));
        tr->used = true;
        tr->id   = t->next_id++;
        strncpy(tr->cls, msg->detections[i].cls, sizeof(tr->cls) - 1);
        tr->pan          = dp[i];
        tr->tilt         = dt[i];
        tr->ang_w        = dw[i];
        tr->hits         = 1;
        tr->last_seen_ms = now;
        out_ids[i]       = tr->id;
        out_confirmed[i] = (tr->hits >= N_INIT);   /* false at hits==1 */

        /* Log why this detection failed to match: the nearest same-class
         * track and the gate it would have needed. A spawn right next to a
         * live track is the association-failure (id churn) signature. */
        float    near_d  = -1.0f;
        uint32_t near_id = 0;
        for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
        {
            const Track *o = &t->tracks[k];
            if (!o->used || o->id == tr->id) continue;
            if (strcmp(o->cls, tr->cls) != 0) continue;
            float d = hypotf(dp[i] - o->pan, dt[i] - o->tilt);
            if (near_d < 0.0f || d < near_d)
            {
                near_d  = d;
                near_id = o->id;
            }
        }
        if (near_d >= 0.0f)
        {
            printf("[TRACKER] spawn id=%u cls=%s pan=%.2f tilt=%.2f "
                   "(nearest id=%u at %.2f deg, gate %.2f)\n",
                   tr->id, tr->cls, (double)tr->pan, (double)tr->tilt,
                   near_id, (double)near_d,
                   (double)fmaxf(GATE_MIN_DEG, GATE_K * dw[i]));
        }
        else
        {
            printf("[TRACKER] spawn id=%u cls=%s pan=%.2f tilt=%.2f\n",
                   tr->id, tr->cls, (double)tr->pan, (double)tr->tilt);
        }
    }

    /* Coast/delete tracks not seen this frame that have aged out. The
     * now >= last_seen_ms test keeps an out-of-order (older) capture time
     * from wrapping the unsigned subtraction into a delete-everything. */
    for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
    {
        Track *tr = &t->tracks[k];
        if (tr->used && tr->id == pinned)
        {
            continue;   /* the lock, not coast time, bounds the pinned track */
        }
        if (tr->used && !trk_matched[k] &&
            now >= tr->last_seen_ms && (now - tr->last_seen_ms) > MAX_COAST_MS)
        {
            printf("[TRACKER] delete id=%u cls=%s unseen=%llums hits=%d\n",
                   tr->id, tr->cls,
                   (unsigned long long)(now - tr->last_seen_ms), tr->hits);
            tr->used = false;
        }
    }
}

int tracker_active_count(const Tracker *t)
{
    if (t == NULL) return 0;
    int n = 0;
    for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
    {
        if (t->tracks[k].used) n++;
    }
    return n;
}
