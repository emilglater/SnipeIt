/**
 * tracker.c
 *
 * Greedy nearest-neighbour tracker in motion-compensated angular space.
 * See tracker.h for the design rationale.
 */

#include "tracker.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Tunables ------------------------------------------------------------ */
#define GATE_MIN_DEG   2.0f    /* minimum association gate (degrees)           */
#define GATE_K         1.5f    /* gate also scales with target angular width   */
#define MAX_COAST_MS   1500u   /* delete a track unseen for this long          */
#define N_INIT         2       /* hits before a track is "confirmed"           */
#define BEARING_ALPHA  0.5f    /* EMA smoothing of the track bearing           */

#define DEG2RAD (0.017453292519943295f)
#define RAD2DEG (57.29577951308232f)

typedef struct
{
    bool     used;
    uint32_t id;
    char     cls[ORIN_CLASS_MAXLEN];
    float    pan;            /* smoothed world bearing, degrees                */
    float    tilt;
    /* EXTENSION POINT: to lead fast slews, add a constant-velocity term here
     *   float vpan, vtilt;  // deg/s, estimated from (new-old)/dt
     * and predict `pan += vpan*dt` before gating in tracker_update(). Only add
     * if measurement shows a target moving more than the gate between frames
     * during a hard slew — greedy-NN without prediction handles ~5-7 fps + the
     * scan's mini-scan pauses fine. This is a local change, not a rewrite. */
    float    ang_w;          /* last angular width, degrees (for gating)       */
    int      hits;
    uint64_t last_seen_ms;
} Track;

struct Tracker
{
    const AimConfig *aim;
    Track            tracks[TRACKER_MAX_TRACKS];
    uint32_t         next_id;   /* stable id counter; 0 is reserved "none"     */
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

static int find_free_or_evict(Tracker *t)
{
    for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
    {
        if (!t->tracks[k].used)
        {
            return k;
        }
    }
    /* Full: evict the track unseen the longest. */
    int      slot   = 0;
    uint64_t oldest = t->tracks[0].last_seen_ms;
    for (int k = 1; k < TRACKER_MAX_TRACKS; k++)
    {
        if (t->tracks[k].last_seen_ms < oldest)
        {
            oldest = t->tracks[k].last_seen_ms;
            slot   = k;
        }
    }
    return slot;
}

void tracker_update(Tracker *t, const OrinDetectionMsg *msg, const PoseEntry *pose,
                    uint32_t *out_ids, bool *out_confirmed)
{
    if (t == NULL || msg == NULL || pose == NULL)
    {
        return;
    }

    const uint64_t now = pose->capture_ts_ms;
    const int      nd  = (msg->num_detections < ORIN_MAX_DETECTIONS)
                            ? msg->num_detections : ORIN_MAX_DETECTIONS;

    float dp[ORIN_MAX_DETECTIONS];   /* detection bearings + width */
    float dt[ORIN_MAX_DETECTIONS];
    float dw[ORIN_MAX_DETECTIONS];
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
        if (bi < 0) break;

        Track *tr = &t->tracks[bk];
        tr->pan  += BEARING_ALPHA * (dp[bi] - tr->pan);
        tr->tilt += BEARING_ALPHA * (dt[bi] - tr->tilt);
        tr->ang_w        = dw[bi];
        tr->hits        += 1;
        tr->last_seen_ms = now;
        det_taken[bi]    = true;
        trk_matched[bk]  = true;
        out_ids[bi]       = tr->id;
        out_confirmed[bi] = (tr->hits >= N_INIT);
    }

    /* Spawn a new track for every unmatched detection. */
    for (int i = 0; i < nd; i++)
    {
        if (det_taken[i]) continue;
        int slot = find_free_or_evict(t);
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
    }

    /* Coast/delete tracks not seen this frame that have aged out. */
    for (int k = 0; k < TRACKER_MAX_TRACKS; k++)
    {
        Track *tr = &t->tracks[k];
        if (tr->used && !trk_matched[k] && (now - tr->last_seen_ms) > MAX_COAST_MS)
        {
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
