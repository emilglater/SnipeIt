# Pi → App team: post-EdgeTPU brief — detection/tracking contract changes

You are the Claude working on the SnipeIt viewer app. This is a handoff from the
Pi-side team. The Pi's detection architecture was replaced; the wire protocol to
the app was kept deliberately compatible, but the **semantics** of some fields
changed, and we believe leftover TPU-era logic in the app is now fighting the
new pipeline. First live end-to-end run (indoor) showed symptoms consistent
with that. Please read this, answer the questions at the end, and make the app
changes listed — we are holding all Pi-side tuning until your changes land and
we re-run outdoors.

## 1. What changed behind the protocol (context)

OLD (TPU era): Python + EdgeTPU ran detection ON the Pi per frame; detections
went to the app raw and per-frame; there was no tracking anywhere on the Pi.
Any smoothing/ID logic the app added back then was compensating for that.

NEW: the Pi streams 1080p H.265 to a Jetson Orin over a direct GigE link; the
Orin runs TensorRT detection (stateless, per-frame) and pushes results back to
the Pi; the Pi then:
- joins each detection to the **servo pose at the frame's capture time**,
- runs a **motion-compensated tracker** (association in world-angle space, so
  IDs survive camera pans; tracks coast up to 1.5 s through missed frames),
- drives the servos itself when locked (lock-follow is on the Pi, closed-loop
  through the tracker),
- forwards detections to the app over the same WebSocket, same message type.

**Net: the Pi now owns tracking and aiming. The app's job is display + operator
commands.** Any app-side tracker/re-ID/coasting layer is now redundant and
actively harmful (two trackers disagreeing = ID churn on screen).

## 2. The exact wire contract (what the app receives today)

Transport unchanged: RTSP video `rtsp://<pi>:8554/stream`, WebSocket on `:8555`.

```json
{"type":"target_detection","timestamp_ms":123456789,"detections":[
  {"id":"7","class":"HUMAN","confidence":0.90,
   "bbox":{"x":868,"y":857,"width":304,"height":219},"confirmed":true}
]}
```

Field-by-field, vs the TPU era:

- **`id` — SEMANTICS CHANGED.** It is now a **stable track id** from the Pi
  tracker: the same physical target keeps the same id across frames, including
  through short detection gaps (≤1.5 s coast) and while the camera pans. Render
  it verbatim. It is a small integer as a string and grows monotonically over
  the session (a target that disappears >1.5 s and returns gets a NEW id — that
  is correct behaviour, not a bug).
- **`confirmed` — NEW, optional, per detection.** `true` once a track has ≥2
  consecutive hits. The Pi only accepts a lock on confirmed tracks. Suggested
  use: dim/ghost unconfirmed boxes and disable the lock affordance on them.
- **Fallback mode — `confirmed` ABSENT:** if the Pi's frame_id→pose join misses
  (rare), the message carries the Orin's raw per-frame ids (1-based index,
  restarts every frame) and **no `confirmed` field**. Treat such messages as
  overlay-only: ids in them are meaningless across frames. Detect this case by
  the absence of `confirmed`, not by id values.
- **`timestamp_ms` — SEMANTICS CHANGED.** It is the **Orin's monotonic clock**
  (ms at inference completion). It is NOT epoch time and is NOT comparable to
  the app's clock or to the TPU-era value. If the app uses it for staleness or
  sync, stop (see questions).
- **`class`** — final contract is `"HUMAN"` (and later `"DRONE"`). Currently
  the Orin's model may emit other labels (you saw chair / tennis racket) —
  that's an Orin-side threshold/class-filter tune that is already on their
  list, NOT something the app should permanently filter. Just don't crash on
  unknown class strings meanwhile.
- **`bbox`** — unchanged: pixels in the 1920x1080 source frame, x/y = top-left.
- **Empty `detections: []` messages ARE sent** and mean "clear all boxes".
  Every Orin result is forwarded, detections or not.
- **Rate/latency — CHANGED.** TPU era was local and fast. Now expect **2–8
  messages/s** (indoor low light = slow end; outdoor daylight = ~7–8/s), and
  each detection lags the live scene by roughly encode + inference + return
  (hundreds of ms), which is NOT the same latency as the RTSP video path — so
  a box drawn on the current video frame can trail a moving target even when
  everything is healthy.

Lock command (unchanged shape, one requirement): `select_target` with
`action:"lock"` and `target_id` (or `id`) = **the `id` string from these
messages, verbatim**. If the id doesn't match a currently-confirmed track, the
Pi falls back to locking the highest-confidence detection — so an app that
sends its OWN internal id instead of the wire id would appear to "lock the
wrong/higher target". `action:"unlock"` releases; any `set_servo_angles`
(manual joystick) also clears the lock.

## 3. Changes we're asking the app to make

1. **Remove/disable the TPU-era tracker workaround** (whatever combination of
   smoothing, ID reassignment, box coasting it does). The Pi tracker replaces
   it. Two trackers stacked is our #1 suspect for the inconsistent IDs seen in
   the indoor run.
2. **Stop generating app-side IDs; display the wire `id` verbatim**, and send
   that exact string back in `select_target.target_id` on lock.
3. **Use `confirmed`**: full-strength box + lockable when `true`; dimmed +
   not lockable when `false`; when the field is absent treat the whole message
   as overlay-only (no lock from it).
4. **Retune staleness for the new rate**: don't expire boxes on a timeout
   shorter than ~1.5–2 s. Clearing is signalled explicitly by an empty
   `detections` array; a timeout should only be a backstop (e.g. 3 s = link
   lost).
5. **Don't interpret `timestamp_ms` as wall-clock** anywhere.
6. Purely-visual interpolation between updates (tweening a box toward its next
   position) is fine if you already have it — but it must not change ids,
   spawn boxes, or keep a box alive past an explicit empty/clear.

## 4. Questions — please answer these in your reply

1. What exactly does the TPU-era tracker workaround do (smoothing? re-ID by
   IoU/position? coasting?), and what were the displayed IDs based on?
2. On lock: what does the app currently send in `select_target` — the wire id
   verbatim, or an internally generated id? (This decides whether the indoor
   "locked but centered elsewhere" symptom could be a silent
   highest-confidence fallback on the Pi.)
3. Does the app apply its own confidence or class filtering before drawing?
   What thresholds?
4. How are boxes time-aligned to video — drawn immediately on arrival over the
   live frame, or with any delay/latency compensation? Do you know your RTSP
   display latency?
5. What clears boxes today: empty message, timeout, or both? What timeout?
6. Is `timestamp_ms` used for anything (staleness, ordering, sync)?
7. Anything else in the app that keyed off the TPU-era behaviour (per-frame
   ids, ~high message rate, epoch timestamps) that we should know about?

## 5. Indoor-run symptoms and our current attribution (for shared context)

| Symptom | Suspected owner |
|---|---|
| Boxes on chair/tennis racket | Orin (confidence threshold + class filter) — already flagged to them |
| IDs change for the same person | App (TPU-era ID/tracker layer vs Pi tracker) — this brief |
| Laggy/misplaced boxes | Mixed: indoor low-light fps (Pi/lighting) + video-vs-detection latency skew (ask Q4) + app staleness handling |
| Lock centers above the chosen target | Pi (aiming geometry/FOV) — ours, we tune after the outdoor run |
| Un-locked overlay motion not smooth | Expected at 2–8 msg/s; app tweening (item 6) can mask it |

Plan of record: app makes the changes above → we re-run outdoors in daylight →
only then does the Pi side tune (aiming vertical bias, thresholds with Orin,
tracker gates). Please send back the Q&A answers plus anything in your
implementation you think we've mis-modelled.
