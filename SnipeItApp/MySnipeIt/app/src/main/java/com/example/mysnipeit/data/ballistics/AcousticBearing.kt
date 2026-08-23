package com.example.mysnipeit.data.ballistics

import com.example.mysnipeit.data.models.AcousticEvent

/**
 * Convert a raw [AcousticEvent]'s mic-frame azimuth into a true-north
 * world bearing (0–360°). Returns null when the event is absent,
 * flagged `valid = false` by the Pi, or the system hasn't been
 * calibrated yet — in all those cases the upstream alert UI should
 * show nothing rather than compute a bearing from garbage.
 *
 * Geometry, single line of math:
 *
 *   world = tripod_world_bearing_at_calibration + event.azimuth
 *
 * Where `tripodWorldBearingDeg` is the operator-captured compass
 * reading from when the camera arm was centred at servo 90° (mic 0° /
 * tripod-forward direction) — see
 * [com.example.mysnipeit.viewmodel.SniperViewModel.calibrateTripodWorldBearing].
 *
 * Why not the live compass: the compass is mounted on the moving
 * camera head, so its reading reflects where the CAMERA is currently
 * pointing — not where the mic array (on the fixed tripod) is
 * pointing. The mic array's world orientation doesn't change as the
 * head moves; it's only the captured-at-setup reading that matters.
 *
 * The function lives next to [localizeTarget] because both follow the
 * same null-on-missing-input contract; it isn't strictly "ballistics"
 * but the package is the natural home for rig-geometry-aware sensor
 * math.
 */
fun worldBearingFromAcousticEvent(
    event: AcousticEvent?,
    tripodWorldBearingDeg: Double?,
): Double? {
    if (event == null || !event.valid) return null
    if (tripodWorldBearingDeg == null) return null
    return normalizeDeg(tripodWorldBearingDeg + event.azimuthDeg.toDouble())
}
