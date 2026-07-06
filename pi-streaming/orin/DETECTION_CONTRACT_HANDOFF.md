# App-Side Detection Contract — Handoff for the Pi Team

**Branch:** `detection-contract` (off `stream-resilience`)
**In reply to:** your "post-EdgeTPU brief — detection/tracking contract changes"
**Status:** all six changes in your §3 are done, plus one field-name fix we caught in your §2 lock contract. Answers to your seven questions are below.

---

## TL;DR

The app is now a **pure pass-through display**. The entire app-side tracker (re-ID, smoothing, coasting, burst pacer) is deleted — the Pi tracker is trusted end-to-end. Wire `id` is rendered and sent back verbatim, `confirmed` gates lockability, empty arrays clear immediately, staleness is a 3 s link-lost backstop, `timestamp_ms` is ignored. Boxes snap raw (no tween) so your outdoor run sees the tracker's true motion.

**Most important:** the app was doing TWO things that produced your "locked but centered elsewhere" symptom — (a) sending its *own* internal id instead of the wire id, and (b) sending the field as `targetId` instead of `target_id`/`id`. Both are fixed. The lock should now hit the chosen track.

---

## Answers to your 7 questions

**Q1 — What did the TPU-era tracker do, and what were the displayed IDs?**
Four stacked layers, all now removed:
- **Burst pacer** — a channel + coroutine coalesced detection bursts to the most-recent frame, throttled to ≥100 ms between UI updates (built for the TPU dumping ~130 detections in 300 ms).
- **Re-ID by IoU** — greedy bbox-overlap matching (threshold 0.3), assigning the app's **own** ids `T1, T2, …`. The wire `id` was parsed then **discarded**.
- **EMA smoothing** — bbox + confidence, alpha 0.7.
- **Coasting** — a track was kept alive up to 600 ms without a fresh match.
- **Displayed IDs were the app's `T1/T2`, not your wire id.**

**Q2 — On lock, what did the app send?** The app's **internal** id (`T1`), via a field named **`targetId`**. So it was wrong on *both* the value (internal vs wire) and the field name (`targetId` vs your `target_id`/`id`). **This is your "locked but centered elsewhere" = highest-confidence fallback.** Both are now fixed (see changes 2 and 7).

**Q3 — Own confidence/class filtering before drawing?** **None.** No threshold, no class filter — every detection with a valid bbox was drawn. (That's why chair/tennis-racket boxes appeared; drawing everything is the app, thresholding is yours — already on your list.) We still don't filter; we just tolerate any class string.

**Q4 — How are boxes time-aligned to video?** Drawn **immediately on arrival** over the current video frame — **no** latency compensation, **no** timestamp alignment. We do **not** measure RTSP display latency. So detection lag + independent video latency can make a box trail a moving target; we don't correct for it.

**Q5 — What clears boxes now?** Explicit empty `detections: []` → cleared **immediately**. The 3 s staleness timeout is a **link-lost backstop only** (no message at all for 3 s). (Previously: an empty message cleared ~600 ms late via the coast, and staleness was 5 s.)

**Q6 — Is `timestamp_ms` used?** **No.** It was stored on the model but read nowhere for logic (staleness used the app's own receive clock). We've now dropped the field entirely. Nothing in the app interprets it as wall-clock.

**Q7 — Anything else keyed off TPU behavior?** Yes, all now removed/retuned: the pacer assumed high burst rates (inert at 2–8/s); the **600 ms coast duplicated and could fight your 1.5 s coast** (app dropping a track you were still coasting → flicker); EMA lagged boxes; the 5 s staleness exceeded your 3 s backstop.

---

## Changes made (your §3, plus the §2 fix)

1. **Removed the app tracker** — pacer, IoU re-ID, EMA, coast, `T1/T2` counter. ~200 lines deleted.
2. **Wire `id` verbatim** — parsed and displayed as-is; it *is* the value sent back on lock.
3. **`confirmed` used** — `true` → full-strength box, lockable. `false` → **ghosted (dim 0.4α)**, and the LOCK button is replaced by an **"UNCONFIRMED"** label (lock affordance disabled). **Absent → parsed as `null` → whole message treated as fallback/overlay-only**, nothing lockable. We detect fallback by the null, not by id values, exactly as you specified.
4. **Staleness retuned** — empty array clears immediately; 3 s timeout is link-lost backstop only.
5. **`timestamp_ms` ignored** — field dropped; never read as wall-clock.
6. **Tween stripped** — boxes snap to your reported position (raw motion for the outdoor run). Pure-visual tween can return afterward; it will never change ids/spawn/outlive a clear when it does.
7. **(§2 fix) Lock field name** — now sends **`target_id`** *and* `id` (same value) + `action`, instead of `targetId`. See "Needs your confirmation" below.

Unconfirmed-vs-confirmed, at a glance:
| `confirmed` | Box | Lockable |
|---|---|---|
| `true` | full strength | yes |
| `false` | ghosted (0.4α) + "UNCONFIRMED" | no |
| absent (`null`) | drawn, overlay-only | no (whole message) |

---

## ⚠️ Needs your confirmation

**Lock payload field name.** Your brief says the Pi reads `target_id` (or `id`). The app now sends **both** (same value) to be safe:

```json
{"type":"command","command":"select_target",
 "params":{"target_id":"7","id":"7","action":"lock"},
 "timestamp":1718...}
```

Please confirm your lock-follow parser reads `target_id` or `id` from `params` (not `targetId`). If it wants something else, tell us the exact key. `action` is `"lock"` / `"unlock"`; a `set_servo_angles` (manual) also clears the lock on your side, which we rely on — the app doesn't send an explicit unlock in that case.

---

## What you should see on the outdoor re-run

- **Stable IDs straight from your tracker** — no app-side churn (the two-trackers-fighting cause is gone).
- **Correct-target lock** — the chosen track's id round-trips; no more highest-confidence fallback (assuming the field-name confirmation above).
- **Unconfirmed detections ghosted + non-lockable**; fallback (no-`confirmed`) messages are overlay-only.
- **Raw box motion** — no app tween, so any remaining jitter/lag is unambiguously the tracker's (or the detection-vs-video latency skew from Q4), for you to tune.
- **Clean clears** — boxes vanish the instant you send an empty array; they only persist if messages stop for 3 s (link lost).

---

## Not addressed here (by design / out of scope)

- **Detection-vs-video latency compensation** (your Q4) — we don't do it and didn't add it. If the box-trails-target skew matters, that's a separate feature (we'd need a way to age-align detections to displayed video frames). Flag it if you want it.
- **Class filtering** — left off permanently, per your note that it's an Orin threshold/class-filter tune. We just don't crash on unknown class strings.

---

## Branch / build note

`detection-contract` sits on top of `stream-resilience` (the video-resilience work). Both are unmerged pending a real-device build check on our side. When merged, `detection-contract` carries both sets of changes.

*Send back the field-name confirmation and anything you think we've mis-modelled. After your outdoor run, if you want the pure-visual box tween re-added (smoother motion, same ids/lifecycle), it's a quick change.*
