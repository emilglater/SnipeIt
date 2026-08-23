package com.example.mysnipeit.data.models

/**
 * UI-facing acoustic alert. Built from a raw [AcousticEvent] + the
 * current compass heading by [com.example.mysnipeit.viewmodel.SniperViewModel],
 * which also applies the dedupe-then-replace and auto-dismiss policy.
 *
 *  - [bearingDeg] is the WORLD-relative true-north bearing (0-360),
 *    fixed at the moment of detection. Subsequent rig rotation doesn't
 *    move it — the sound source is still where it was in the world.
 *    Only meaningful when [isWorldBearing] is true.
 *  - [isWorldBearing] true when the system was calibrated (and the
 *    calibration hadn't expired) at detection time, so [bearingDeg] is
 *    a real true-north bearing. False when uncalibrated/expired — the
 *    UI then shows [rawAzimuthDeg] as a RELATIVE angle (mic-frame,
 *    "left/right of tripod-forward") instead. The SLEW action works in
 *    either case because it only needs [rawAzimuthDeg].
 *  - [firstSeenAtMs] is the timestamp the alert first appeared. The
 *    20 s auto-dismiss timer counts from here, NOT from the last merge:
 *    a continuously-firing source still clears itself eventually so the
 *    HUD doesn't accumulate stale alerts.
 *  - [lastUpdatedAtMs] is refreshed every time a same-source event
 *    (within the dedupe window) merges in. Useful for a "freshness"
 *    visual hint on the card.
 *  - [isInteractive] decides the render mode: true → full card with
 *    SLEW / DISMISS buttons; false → passive chip (no buttons). Set to
 *    false by the ViewModel whenever a target is currently selected, so
 *    a stray tap can't slew the camera off a locked engagement. Flips
 *    back to true automatically the moment the operator deselects.
 */
data class AudioAlert(
    val bearingDeg: Double,
    val rawAzimuthDeg: Double,
    val isWorldBearing: Boolean,
    val confidence: Float,
    val peakAmplitude: Float,
    val durationMs: Float,
    val firstSeenAtMs: Long,
    val lastUpdatedAtMs: Long,
    val isInteractive: Boolean,
    // When the operator hits SLEW the alert isn't cleared immediately —
    // instead isAccepted flips true and the card briefly shows a
    // "SLEWING…" notice (~1.5 s) before being swept away. Gives the
    // operator visual confirmation that the tap registered and the Pi
    // is now moving, rather than the alert just vanishing.
    val isAccepted: Boolean = false,
    val acceptedAtMs: Long = 0L,
)
