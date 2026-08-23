package com.example.mysnipeit.data.models

import com.google.gson.annotations.SerializedName

/**
 * A single acoustic detection from the Pi's 4-mic TDOA module.
 *
 * Mirrors the Pi-side JSON verbatim:
 *
 * ```
 * {
 *   "type": "acoustic_event",
 *   "timestamp_us": 1234567890,
 *   "azimuth_deg": 42.5,
 *   "confidence": 0.87,
 *   "peak_amplitude": 0.95,
 *   "duration_ms": 3.2,
 *   "valid": true
 * }
 * ```
 *
 * Bearing convention: [azimuthDeg] is in the **mic-array's own frame**, not
 * world-relative. The mic array is bolted to the fixed tripod (under the
 * camera's pan/tilt head) so the camera servos don't affect it, but it
 * does rotate with the tripod itself. To convert to a true-north bearing,
 * combine with the current compass heading and the constant offset between
 * the compass's reference axis and the mic array's reference axis — see
 * `RigGeometry.MIC_ARRAY_OFFSET_DEG` and the bearing helper in the next
 * commit. Until that helper lands, treat [azimuthDeg] as opaque sensor
 * data — don't render it as a world bearing.
 *
 * The [valid] flag gates whether the event is usable, same convention as
 * the other sub-frames. The Pi may emit `valid=false` when the TDOA solver
 * couldn't disambiguate the direction (e.g. reflections / multiple sources
 * confused the cross-correlation).
 */
data class AcousticEvent(
    val type: String? = null,
    @SerializedName("timestamp_us") val timestampUs: Long = 0L,
    @SerializedName("azimuth_deg") val azimuthDeg: Float = 0f,
    val confidence: Float = 0f,
    @SerializedName("peak_amplitude") val peakAmplitude: Float = 0f,
    @SerializedName("duration_ms") val durationMs: Float = 0f,
    val valid: Boolean = false,
)
