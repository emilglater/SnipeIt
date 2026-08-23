package com.example.mysnipeit.data.models

import com.google.gson.annotations.SerializedName

/**
 * Sensor data from the RPi5 sensor pipeline.
 *
 * Mirrors the nested `ddl_frame` JSON the Pi sends:
 *
 * ```
 * {
 *   "type": "sensor_data",
 *   "timestamp": 1748600000000,
 *   "ddl_frame": {
 *     "distance":             { "valid": ..., "distance_m": ..., "status": ..., "precision": ..., "strength": ... },
 *     "temperature_humidity": { "valid": ..., "temperature_c": ..., "humidity_pct": ... },
 *     "servo":                { "horizontal_deg": ..., "vertical_deg": ... },
 *     "gps":                  { "valid": ..., "fix_type": ..., "num_satellites": ..., "latitude_deg": ..., "longitude_deg": ..., "altitude_m": ..., "h_acc_m": ... },
 *     "compass":              { "valid": ..., "raw_x": ..., "raw_y": ..., "raw_z": ..., "temperature_c": ..., "heading_deg": ... },
 *     "wind":                 { "speed_valid": ..., "speed_mps": ..., "direction_valid": ..., "direction_deg": ... }
 *   }
 * }
 * ```
 *
 * Display rule: a sub-frame's data is shown on the dashboard only if the
 * sub-frame is present AND its `valid` flag is true. [ServoFrame] has no
 * `valid` field (matches the C struct on the Pi); it's treated as valid
 * whenever its sub-frame is present, but its two angles are individually
 * nullable so an absent field stays distinguishable from a real 0°.
 * [WindFrame] has TWO valid flags — speed and direction can be valid
 * independently.
 *
 * Quirk: [CompassFrame.headingDeg] is **nullable**. The Pi emits the JSON
 * literal `null` (not a number) when the magnetometer hasn't fixed yet —
 * see the C `build_json` function. Read it via [compassHeadingDeg] which
 * also gates on `valid`, so the future ballistics calc never accidentally
 * treats "no heading" as "heading = 0° (true north)".
 *
 * Fields the Pi sends but that we don't yet display (e.g. distance
 * precision/strength, GPS altitude/h_acc, compass raw_x/y/z, compass
 * temperature) are still parsed into these classes so they're available
 * when the app starts computing its own shooting solution.
 */
data class SensorData(
    val type: String? = null,
    val timestamp: Long = 0L,
    @SerializedName("ddl_frame") val ddlFrame: DdlFrame? = null,
)

/**
 * Container for the six current sensor sub-frames. Each sub-frame is
 * nullable so the Pi can omit a subsystem that's offline / not yet
 * initialized without breaking the parse. Leaf fields inside each
 * sub-frame are non-null per the C-struct contract on the Pi — with the
 * one exception of [CompassFrame.headingDeg], which can be JSON `null`.
 */
data class DdlFrame(
    val distance: DistanceFrame? = null,
    @SerializedName("temperature_humidity") val temperatureHumidity: TempHumidityFrame? = null,
    val servo: ServoFrame? = null,
    val gps: GpsFrame? = null,
    val compass: CompassFrame? = null,
    val wind: WindFrame? = null,
)

/** Laser rangefinder. `valid` gates whether `distanceM` is displayed. */
data class DistanceFrame(
    val valid: Boolean = false,
    @SerializedName("distance_m") val distanceM: Float = 0f,
    val status: Int = 0,
    val precision: Int = 0,
    val strength: Int = 0,
)

/** Environmental temperature + humidity. */
data class TempHumidityFrame(
    val valid: Boolean = false,
    @SerializedName("temperature_c") val temperatureC: Float = 0f,
    @SerializedName("humidity_pct") val humidityPct: Float = 0f,
)

/**
 * Camera mount servo angles. Has NO `valid` field on the Pi side (matches
 * the C struct), so the sub-frame's presence is the only signal there is.
 *
 * Both angles are **nullable**, deliberately. With a non-null `0f` default,
 * a field missing from the JSON was indistinguishable from a servo genuinely
 * sitting at 0° — and 0° is a real position (full down), not a sentinel. Any
 * consumer reading a spurious `0` as an angle would aim the camera at the
 * ground. Same reasoning, and same fix, as [CompassFrame.headingDeg].
 *
 * Read via [servoHorizontalDeg] / [servoVerticalDeg] and treat null as
 * "unknown". Never substitute 0.
 */
data class ServoFrame(
    @SerializedName("horizontal_deg") val horizontalDeg: Float? = null,
    @SerializedName("vertical_deg") val verticalDeg: Float? = null,
)

/** GPS position + fix quality. `valid` gates whether the position is shown. */
data class GpsFrame(
    val valid: Boolean = false,
    @SerializedName("fix_type") val fixType: Int = 0,
    @SerializedName("num_satellites") val numSatellites: Int = 0,
    @SerializedName("latitude_deg") val latitudeDeg: Double = 0.0,
    @SerializedName("longitude_deg") val longitudeDeg: Double = 0.0,
    @SerializedName("altitude_m") val altitudeM: Double = 0.0,
    @SerializedName("h_acc_m") val hAccM: Double = 0.0,
)

/**
 * Magnetometer / compass.
 *
 * `headingDeg` is intentionally **nullable** because the Pi C code emits
 * JSON `null` (not a number) when the magnetometer hasn't fixed yet:
 *
 * ```c
 * if (c->valid && isfinite(c->heading_deg))
 *     snprintf(heading_buf, ..., "%.2f", ...);
 * else
 *     snprintf(heading_buf, ..., "null");          // ← literal null
 * // ...emitted via "heading_deg":%s
 * ```
 *
 * Always read it via [compassHeadingDeg], which checks both `valid` AND
 * non-null heading so a zero magnetometer reading is never confused with
 * "no fix".
 *
 * `rawX/Y/Z` and `temperatureC` are diagnostic fields kept here for the
 * future ballistics calculator (e.g. dynamic declination compensation)
 * but never shown on the dashboard.
 */
data class CompassFrame(
    val valid: Boolean = false,
    @SerializedName("raw_x") val rawX: Int = 0,
    @SerializedName("raw_y") val rawY: Int = 0,
    @SerializedName("raw_z") val rawZ: Int = 0,
    @SerializedName("temperature_c") val temperatureC: Float = 0f,
    @SerializedName("heading_deg") val headingDeg: Float? = null,
)

/**
 * Wind speed + direction.
 *
 * Has TWO independent valid flags — the Pi can report speed without
 * direction (or vice-versa) when one of the sensor channels glitches.
 * Helpers [windSpeedMps] / [windDirectionDeg] each check only their own
 * flag, so the dashboard can show one cell as "—" while the other has
 * a real reading.
 */
data class WindFrame(
    @SerializedName("speed_valid") val speedValid: Boolean = false,
    @SerializedName("speed_mps") val speedMps: Float = 0f,
    @SerializedName("direction_valid") val directionValid: Boolean = false,
    @SerializedName("direction_deg") val directionDeg: Float = 0f,
)

// ---------------------------------------------------------------------------
// Extension helpers — keep dashboard code clean. Each returns the field's
// value only if its sub-frame is present AND valid (or just present for
// the servo case which has no valid flag). Returning null means "don't
// display", and the dashboard renders that as "—".
// ---------------------------------------------------------------------------

fun SensorData?.temperatureC(): Float? =
    this?.ddlFrame?.temperatureHumidity?.takeIf { it.valid }?.temperatureC

fun SensorData?.humidityPct(): Float? =
    this?.ddlFrame?.temperatureHumidity?.takeIf { it.valid }?.humidityPct

fun SensorData?.distanceM(): Float? =
    this?.ddlFrame?.distance?.takeIf { it.valid }?.distanceM

fun SensorData?.gpsLatLon(): Pair<Double, Double>? =
    this?.ddlFrame?.gps?.takeIf { it.valid }?.let { it.latitudeDeg to it.longitudeDeg }

fun SensorData?.gpsSatellites(): Int? =
    this?.ddlFrame?.gps?.takeIf { it.valid }?.numSatellites

// Servo angles — used by the ballistics calculation and the acoustic slew,
// not displayed on the dashboard. No `valid` check because the C struct
// doesn't have one; null means the `servo` object OR the individual field
// was absent from the JSON. Null is NOT 0 — 0° is a real servo position.
fun SensorData?.servoHorizontalDeg(): Float? =
    this?.ddlFrame?.servo?.horizontalDeg

fun SensorData?.servoVerticalDeg(): Float? =
    this?.ddlFrame?.servo?.verticalDeg

/**
 * Compass heading in degrees from magnetic north (0-360°).
 * Returns null when the sub-frame is missing, `valid=false`, OR
 * `heading_deg` is JSON `null`. The ballistics calculator must check
 * for null before using this — a missing heading isn't "0°".
 */
fun SensorData?.compassHeadingDeg(): Float? =
    this?.ddlFrame?.compass?.takeIf { it.valid }?.headingDeg

/** Wind speed in m/s. Gated by `speed_valid`. */
fun SensorData?.windSpeedMps(): Float? =
    this?.ddlFrame?.wind?.takeIf { it.speedValid }?.speedMps

/** Wind direction in degrees from north (0-360°). Gated by `direction_valid`. */
fun SensorData?.windDirectionDeg(): Float? =
    this?.ddlFrame?.wind?.takeIf { it.directionValid }?.directionDeg
