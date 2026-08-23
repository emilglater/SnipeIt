package com.example.mysnipeit.data.models

import com.google.gson.annotations.SerializedName

data class BoundingBox(
    val x: Int,
    val y: Int,
    val width: Int,
    val height: Int
)

/**
 * A detection as forwarded by the Pi. The Pi owns tracking now (motion-
 * compensated, world-angle association), so the app displays these verbatim
 * — no app-side re-ID / smoothing / coasting.
 *
 * @property id     STABLE track id from the Pi tracker: the same physical
 *                  target keeps this id across frames, through short detection
 *                  gaps (Pi coasts ≤1.5 s) and camera pans. Render as-is and
 *                  send it back verbatim in `select_target.target_id` on lock.
 *                  Monotonic over a session; a target gone >1.5 s that returns
 *                  gets a NEW id (correct, not a bug).
 * @property confirmed  Per-detection track confidence gate. `true` once a
 *                  track has ≥2 consecutive hits — the Pi only accepts a lock
 *                  on confirmed tracks, so the UI dims unconfirmed boxes and
 *                  disables the lock affordance on them. **null = fallback
 *                  mode**: the message arrived without the field, meaning the
 *                  ids in it are the Orin's raw per-frame indices (meaningless
 *                  across frames) — treat the whole batch as overlay-only, no
 *                  lock. Detect fallback by null here, not by id values.
 */
data class DetectedTarget(
    val id: String,
    @SerializedName("class")
    val targetType: String,  // "HUMAN", "DRONE", etc. (tolerate any string)
    val confidence: Float,
    val bbox: BoundingBox,
    val confirmed: Boolean? = null,
)

enum class TargetType {
    HUMAN,
    VEHICLE,
    STRUCTURE,
    UNKNOWN
}