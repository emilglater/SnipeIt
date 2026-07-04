# Pi-side tracker — design sketch

Goal: turn the Orin's **per-frame detection indices** into **stable track ids**
that persist across frames, so the operator can "lock onto target #3" and follow
*that* physical target, and the app shows consistent ids. The Orin stays
stateless; the Pi owns tracking (it has the pose).

## The core idea: track in ANGULAR (world) space, not image space
The camera pans/tilts constantly — fast during the serpentine scan, and while
following a lock. So a *stationary* target sweeps across the image frame-to-frame.
An image-space IoU/box tracker (ByteTrack/SORT as-is) would break: the same
target's box jumps between frames purely from camera motion.

We already have the fix: the `frame_id→pose` ring gives the camera pan/tilt at
each frame's capture, and `aim_compute` converts `bbox + pose → absolute
pan/tilt angle`. So we **motion-compensate**: map every detection to its absolute
world bearing `(abs_pan, abs_tilt)` and **associate in that space**. A target
that's still in the world sits at a fixed bearing even as its image box moves —
associations become stable across scan and follow.

- `abs_pan  = capture_pan  + aim.pan_offset_deg`   (unclamped; from aim_compute)
- `abs_tilt = capture_tilt + aim.tilt_offset_deg`
- angular size `ang_w = 2·atan( (bbox_w/frame_w)·tan(HFOV/2) )` — used only to
  scale the association gate.

## Algorithm: greedy nearest-neighbour in angular space (Norfair-like)
Target counts are small (≤ a handful), so full ByteTrack/Kalman/Hungarian is
overkill. A greedy nearest-neighbour associator with a proper track lifecycle is
simpler, deterministic, and plenty for this. (We can add a constant-velocity
predictor later if fast bearings need lead.)

Per detection **message** (one frame), when the pose join HIT (see misses below):
1. For each detection `i`: compute `(abs_pan_i, abs_tilt_i, ang_w_i)`, class.
2. Build the cost matrix `cost[i][t] = angular_distance(det_i, track_t)` for
   tracks of the **same class**; gate out pairs beyond
   `GATE = max(GATE_MIN_DEG, GATE_K · ang_w_i)`.
3. Greedy match: repeatedly take the globally smallest un-gated cost, bind that
   (det,track), remove both, until none remain.
4. **Matched** track ← update bearing (EMA smooth), bbox, confidence, class;
   `hits++`, `misses=0`, `last_seen_ms = frame_ts`.
5. **Unmatched detections** → spawn a new track: fresh monotonic `id`,
   `hits=1`, `misses=0`.
6. **Unmatched tracks** → `misses++`; delete when
   `frame_ts - last_seen_ms > MAX_COAST_MS` (time-based, robust to irregular fps).

`angular_distance` = hypot(Δpan, Δtilt) in degrees (small-angle; good enough).
Bearing smoothing: `bearing = (1-α)·bearing + α·measurement`, `α≈0.5`.

## Stable id → app + lock
- Each detection gets its matched track's stable id. `build_app_detection_json`
  emits that id in the `"id"` field (as a string) instead of the Orin's
  per-frame index. **App contract unchanged** — same schema, the id is just now
  stable across frames.
- Lock-by-id (`select_detection_index`) matches the operator's locked id against
  the **stable** id. So "lock target 3" follows track 3 across frames; if track 3
  isn't in the current frame, no servo update (already the behaviour).
- Fallback when nothing is locked stays "highest confidence".

## Lifecycle / tunables (proposed defaults)
| Param | Default | Meaning |
|---|---|---|
| `MAX_TRACKS` | 32 | matches `ORIN_MAX_DETECTIONS` |
| `GATE_MIN_DEG` | 2.0° | min association gate |
| `GATE_K` | 1.5 | gate scales with target angular width |
| `MAX_COAST_MS` | 1500 | delete a track unseen this long |
| `N_INIT` | 2 | hits before a track is "confirmed" |
| bearing `α` | 0.5 | EMA smoothing |

Open question: **output tentative tracks?** Proposed — output *all* associated
detections with their id immediately (responsive for a spotter), but only allow
**locking** a *confirmed* track (`hits ≥ N_INIT`) so a one-frame false positive
can't grab the servos. (Easy to change to confirmed-only output.)

## Pose MISS handling
Tracking needs the pose for the angular mapping. On a miss (rare — the sender
tags every frame) we **skip the tracker update** for that frame and forward the
detections to the app with the Orin's per-frame id as a fallback (a momentary id
blip, no servo action — matches the existing skip-on-miss servo behaviour).
Tracks simply coast (their `misses`/coast timer is time-based, so one skipped
frame is harmless).

## Where it lives / threading
- New module `orin/tracker.{h,c}`. Depends on `aiming` (geometry),
  `detection_msg`, `pose_ring`.
- Owned by `ddl_bridge`: `Tracker* tracker;` created in `ddl_bridge_start`,
  destroyed in `ddl_bridge_stop`.
- All tracker state is touched **only on the receiver thread** (inside
  `orin_detection_handler`), so it needs no lock of its own.
- Handler flow becomes: pose HIT → `tracker_update()` fills a stable-id array →
  build app JSON with stable ids → servo select/aim by stable id. pose MISS →
  app JSON with per-frame ids, no tracker, no servo.

## Proposed API
```c
#define TRACKER_MAX_TRACKS 32
typedef struct Tracker Tracker;

Tracker *tracker_create(const AimConfig *aim);   /* borrows aim (geometry) */
void     tracker_destroy(Tracker *t);

/* Update with one frame's detections + its capture pose (must be a join HIT).
 * Writes the stable track id for detections[i] into out_ids[i]; sets
 * out_confirmed[i] = true if that track has hits >= N_INIT. Arrays are caller-
 * sized to msg->num_detections. */
void tracker_update(Tracker *t, const OrinDetectionMsg *msg,
                    const PoseEntry *pose,
                    uint32_t *out_ids, bool *out_confirmed);
```

## Test plan (standalone, before wiring)
`test_tracker.c`: synthetic frames with known bearings + injected camera-pose
changes, asserting: (a) a world-stationary target keeps ONE id while the camera
pans (motion comp works); (b) two targets keep distinct ids; (c) a target that
disappears past `MAX_COAST_MS` is dropped and a reappearance gets a new id;
(d) crossing targets don't swap ids within the gate. Then an integration check
in the live loop (ids stable across frames).
```
```
