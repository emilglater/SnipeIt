package com.example.mysnipeit.data.ballistics

import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.hypot
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Firing solution as the sniper would read it off the HUD. The bearing /
 * range are recomputed from the SNIPER's GPS (not the Pi's), so this is
 * the second half of the two-piece calculator:
 *
 *   Pi sensors ──[localizeTarget]──► TargetLocation
 *                                    │
 *               sniper GPS, loadout, atmosphere
 *                                    │
 *                                    ▼
 *                            [solveFiringSolution]──► FiringSolution
 *
 * All angles are in DEGREES (no MIL / MOA) per product spec.
 *
 * @property rangeM           horizontal ground distance from sniper to target
 * @property slantRangeM      straight-line distance (3D)
 * @property azimuthDeg       true-north bearing from sniper, 0–360
 * @property lookAngleDeg     angle from the sniper's horizon UP to the target's
 *                            apparent position. + when the target is uphill,
 *                            − when downhill. Used by the HUD compass to show
 *                            "the target is this much above me", independent of
 *                            the much smaller hold-over correction below.
 * @property elevationDeg     hold-over ABOVE the line of sight to the target.
 *                            The operator aims their reticle this far above
 *                            the target's apparent position. + up, − down.
 * @property windageDeg       horizontal aim correction. + means hold RIGHT.
 * @property timeOfFlightS    bullet flight time in seconds
 * @property impactVelocityMps remaining velocity at target
 * @property confidence       0..1, lower when any optional input is missing
 *                            (no wind, no atmosphere) and the solver fell
 *                            back to ICAO standard / zero wind.
 */
data class FiringSolution(
    val rangeM: Double,
    val slantRangeM: Double,
    val azimuthDeg: Double,
    val lookAngleDeg: Double,
    val elevationDeg: Double,
    val windageDeg: Double,
    val timeOfFlightS: Double,
    val impactVelocityMps: Double,
    val confidence: Float,
)

// ---------------------------------------------------------------------------
// Physical constants
// ---------------------------------------------------------------------------
private const val GRAVITY_MPS2 = 9.80665
private const val EARTH_RADIUS_M = 6_371_000.0

/** ICAO standard atmosphere at sea level — used when temp/humidity missing. */
private const val ICAO_TEMP_C = 15.0
private const val ICAO_PRESSURE_PA = 101_325.0

/**
 * Conversion: the G1 reference projectile has a sectional density of
 * 1.0 lb/in². To use a G1 BC (conventionally published in lb/in²) in
 * SI drag equations we multiply by 703.07 to get kg/m².
 */
private const val LB_PER_SQIN_TO_KG_PER_SQM = 703.07

/** Specific gas constant for dry air, J/(kg·K). */
private const val R_DRY_AIR = 287.058

/** Ratio of specific heats for air — used in the speed-of-sound formula. */
private const val GAMMA_AIR = 1.4

/**
 * Maximum slant range we'll attempt to solve. Anything beyond is way past
 * the operational envelope of the cartridges in [BallisticProfiles] and
 * the integration cost grows linearly with range.
 */
private const val MAX_SOLVE_RANGE_M = 3_000.0

/** Trajectory integration time step. 1 ms gives < 0.5% error vs analytical drag. */
private const val INTEGRATION_DT_S = 0.001

// ---------------------------------------------------------------------------
// Public solver
// ---------------------------------------------------------------------------

/**
 * Compute the firing solution from the sniper's POV.
 *
 * The flow:
 *  1. Geodetic bearing + range from sniper to [target] (equirectangular —
 *     same projection [localizeTarget] uses; sub-cm accuracy below 5 km).
 *  2. Atmosphere model: temp + humidity → air density; temp → speed of
 *     sound. Falls back to ICAO standard atmosphere if either is null.
 *  3. Bisect on the barrel angle until the bullet passes through the target
 *     point. Trajectory is integrated forward with a small time step using
 *     the G1 drag table — no closed-form, but cheap and stable. The
 *     rifle's sight-height offset is baked into the integration's start Y,
 *     so the rifle's zero range only affects the implicit sight angle of
 *     the scope (computable via [sightAngleRad] for diagnostics).
 *  4. Subtract the look angle (sniper → target) from the barrel angle to
 *     get the hold-over the operator actually dials/holds.
 *  5. Crosswind drift via the standard "lag-time" formula: drift ≈
 *     v_wind_⊥ · (TOF − range / v_muzzle).
 *
 * Returns null if no barrel angle within ±28° produces a hit at the
 * requested range (target out of reach, or above the maximum-elevation
 * arc). [confidence] is reduced when wind / atmosphere inputs are
 * missing so the UI can surface "TARGETING DEGRADED" if it wants to.
 *
 * @param sniperAltM may be approximate — short ranges, the only error this
 *  introduces is in the look-angle to the target. If unknown, pass the Pi's
 *  altitude or 0 and accept the small inaccuracy.
 */
fun solveFiringSolution(
    sniperLatDeg: Double,
    sniperLonDeg: Double,
    sniperAltM: Double,
    target: TargetLocation,
    cartridge: com.example.mysnipeit.data.models.CartridgeProfile,
    rifle: com.example.mysnipeit.data.models.RifleProfile,
    windSpeedMps: Float? = null,
    windDirectionDeg: Float? = null,
    temperatureC: Float? = null,
    humidityPct: Float? = null,
): FiringSolution? {
    // 1. Sniper → target geometry
    val (groundRangeM, azimuthDeg) =
        bearingAndRange(sniperLatDeg, sniperLonDeg, target.latitudeDeg, target.longitudeDeg)
    val verticalM = target.altitudeM - sniperAltM
    val slantRangeM = hypot(groundRangeM, verticalM)
    if (slantRangeM > MAX_SOLVE_RANGE_M || slantRangeM < 1.0) return null
    val lookAngleRad = atan2(verticalM, groundRangeM)

    // 2. Atmosphere
    val tempC = temperatureC?.toDouble() ?: ICAO_TEMP_C
    val rh = humidityPct?.toDouble() ?: 0.0
    val airDensity = airDensityKgPerM3(tempC = tempC, humidityPct = rh, altitudeM = sniperAltM)
    val soundSpeed = soundSpeedMps(tempC = tempC)

    // 3. Sight angle — the angle the barrel sits ABOVE the scope's LOS
    //    when the rifle was zeroed at its nominal range. This is part of
    //    what the operator is NOT holding for (it's already baked into the
    //    scope's zero), so we subtract it from the barrel angle to get the
    //    actual hold-over the operator must apply.
    val sightAngle = sightAngleRad(
        cartridge = cartridge,
        rifle = rifle,
        airDensity = airDensity,
        soundSpeed = soundSpeed,
    ) ?: return null

    // 4. Bisect on barrel angle until the bullet passes through the target.
    val solution = findBarrelAngle(
        targetGroundM = groundRangeM,
        targetVerticalM = verticalM,
        cartridge = cartridge,
        sightHeightM = rifle.sightHeightMm / 1000.0,
        airDensity = airDensity,
        soundSpeed = soundSpeed,
    ) ?: return null

    // 5. Hold-over = barrel angle − sight angle − look angle. At the zero
    //    range with a level target, barrel angle ≈ sight angle and the
    //    look angle is 0, so hold-over is correctly ~0.
    val elevationDeg = Math.toDegrees(solution.barrelAngleRad - sightAngle - lookAngleRad)

    // 5. Windage from crosswind component (lag-time formula).
    val windageDeg = windageDeg(
        windSpeedMps = windSpeedMps?.toDouble(),
        windDirectionDeg = windDirectionDeg?.toDouble(),
        shotAzimuthDeg = azimuthDeg,
        rangeM = groundRangeM,
        timeOfFlightS = solution.tof,
        muzzleVelocity = cartridge.muzzleVelocityMps,
    )

    val confidence = computeConfidence(
        windKnown = windSpeedMps != null && windDirectionDeg != null,
        atmosphereKnown = temperatureC != null && humidityPct != null,
        range = slantRangeM,
    )

    return FiringSolution(
        rangeM = groundRangeM,
        slantRangeM = slantRangeM,
        azimuthDeg = azimuthDeg,
        lookAngleDeg = Math.toDegrees(lookAngleRad),
        elevationDeg = elevationDeg,
        windageDeg = windageDeg,
        timeOfFlightS = solution.tof,
        impactVelocityMps = solution.impactVelocity,
        confidence = confidence,
    )
}

// ---------------------------------------------------------------------------
// Geodesy: equirectangular bearing + range (matches TargetLocalizer's projection)
// ---------------------------------------------------------------------------

private fun bearingAndRange(
    fromLatDeg: Double, fromLonDeg: Double,
    toLatDeg: Double, toLonDeg: Double,
): Pair<Double, Double> {
    val fromLatRad = Math.toRadians(fromLatDeg)
    val dLatRad = Math.toRadians(toLatDeg - fromLatDeg)
    val dLonRad = Math.toRadians(toLonDeg - fromLonDeg)
    val northM = dLatRad * EARTH_RADIUS_M
    val eastM = dLonRad * EARTH_RADIUS_M * cos(fromLatRad)
    val range = hypot(northM, eastM)
    val bearing = ((Math.toDegrees(atan2(eastM, northM)) % 360.0) + 360.0) % 360.0
    return range to bearing
}

// ---------------------------------------------------------------------------
// Atmosphere
// ---------------------------------------------------------------------------

/**
 * Air density in kg/m³ accounting for temperature, humidity, and altitude.
 *
 *  - Pressure from the simplified barometric formula (constant-temp
 *    troposphere is fine for the sub-km altitudes operators will see).
 *  - Magnus saturation pressure for humidity → partial water-vapor pressure.
 *  - Density: dry-air partial + water-vapor partial via R_v (more accurate
 *    than the common "1 − 0.378 e/p" shortcut for humid days).
 */
internal fun airDensityKgPerM3(tempC: Double, humidityPct: Double, altitudeM: Double): Double {
    val tempK = tempC + 273.15
    // Barometric: p(h) = p0 * exp(-h / H), H ≈ 8434 m at 15°C — close enough.
    val pressurePa = ICAO_PRESSURE_PA * exp(-altitudeM / 8434.5)
    // Saturation vapor pressure (Magnus, valid −45..60°C), in Pa.
    val es = 611.21 * exp(17.502 * tempC / (tempC + 240.97))
    val e = (humidityPct.coerceIn(0.0, 100.0) / 100.0) * es
    val pd = pressurePa - e
    val R_VAPOR = 461.495
    return pd / (R_DRY_AIR * tempK) + e / (R_VAPOR * tempK)
}

internal fun soundSpeedMps(tempC: Double): Double =
    sqrt(GAMMA_AIR * R_DRY_AIR * (tempC + 273.15))

// G1 standard drag table, 79 points, Mach 0 to 5.
//
// Replaced 03/08/2026. The previous table paired correct G1 Cd values
// with the wrong Mach axis: its subsonic Cd values were mapped onto the
// supersonic range, so drag was under-applied by up to 34 % at a
// kilometre. Verified against an independent reference calculator, which
// agrees with this table to within 2-4 % across Mach 1.0 to 2.4.
// Source: standard G1 (Ingalls) drag function.
private val G1_TABLE_MACH = doubleArrayOf(
    0.000, 0.050, 0.100, 0.150, 0.200, 0.250, 0.300, 0.350,
    0.400, 0.450, 0.500, 0.550, 0.600, 0.700, 0.725, 0.750,
    0.775, 0.800, 0.825, 0.850, 0.875, 0.900, 0.925, 0.950,
    0.975, 1.000, 1.025, 1.050, 1.075, 1.100, 1.125, 1.150,
    1.200, 1.250, 1.300, 1.350, 1.400, 1.450, 1.500, 1.550,
    1.600, 1.650, 1.700, 1.750, 1.800, 1.850, 1.900, 1.950,
    2.000, 2.050, 2.100, 2.150, 2.200, 2.250, 2.300, 2.350,
    2.400, 2.450, 2.500, 2.600, 2.700, 2.800, 2.900, 3.000,
    3.100, 3.200, 3.300, 3.400, 3.500, 3.600, 3.700, 3.800,
    3.900, 4.000, 4.200, 4.400, 4.600, 4.800, 5.000,
)
private val G1_TABLE_CD = doubleArrayOf(
    0.2629, 0.2558, 0.2487, 0.2413, 0.2344, 0.2278, 0.2214, 0.2155,
    0.2104, 0.2061, 0.2032, 0.2020, 0.2034, 0.2165, 0.2230, 0.2313,
    0.2417, 0.2546, 0.2706, 0.2901, 0.3136, 0.3415, 0.3734, 0.4084,
    0.4448, 0.4805, 0.5136, 0.5427, 0.5677, 0.5883, 0.6053, 0.6191,
    0.6393, 0.6518, 0.6589, 0.6621, 0.6625, 0.6607, 0.6573, 0.6528,
    0.6474, 0.6413, 0.6347, 0.6280, 0.6210, 0.6141, 0.6072, 0.6003,
    0.5934, 0.5867, 0.5804, 0.5743, 0.5685, 0.5630, 0.5577, 0.5527,
    0.5481, 0.5438, 0.5397, 0.5325, 0.5264, 0.5211, 0.5168, 0.5133,
    0.5105, 0.5084, 0.5067, 0.5054, 0.5040, 0.5030, 0.5022, 0.5016,
    0.5010, 0.5006, 0.4998, 0.4995, 0.4992, 0.4990, 0.4988,
)

internal fun g1Cd(mach: Double): Double {
    if (mach <= G1_TABLE_MACH.first()) return G1_TABLE_CD.first()
    if (mach >= G1_TABLE_MACH.last()) return G1_TABLE_CD.last()
    // Linear search is fine — 55 entries, called ~500x per shot.
    for (i in 1 until G1_TABLE_MACH.size) {
        if (mach <= G1_TABLE_MACH[i]) {
            val m0 = G1_TABLE_MACH[i - 1]
            val m1 = G1_TABLE_MACH[i]
            val frac = (mach - m0) / (m1 - m0)
            return G1_TABLE_CD[i - 1] + frac * (G1_TABLE_CD[i] - G1_TABLE_CD[i - 1])
        }
    }
    return G1_TABLE_CD.last()
}

// ---------------------------------------------------------------------------
// Trajectory integration (point-mass with G1 drag)
// ---------------------------------------------------------------------------

/**
 * Result of a single integration pass.
 *  - reachedRange = true if we hit [stopAtRangeM] before the bullet dropped
 *    out the bottom. dropAtRangeM is then the bullet's vertical position
 *    relative to the muzzle when it crossed that range.
 */
private data class TrajResult(
    val reachedRange: Boolean,
    val dropAtRangeM: Double,
    val tof: Double,
    val impactVelocity: Double,
)

private fun integrateTrajectory(
    barrelAngleRad: Double,
    muzzleVelocity: Double,
    bcG1: Double,
    sightHeightM: Double,
    airDensity: Double,
    soundSpeed: Double,
    stopAtRangeM: Double,
    abortBelowM: Double = -50.0,
): TrajResult {
    val bcMetric = bcG1 * LB_PER_SQIN_TO_KG_PER_SQM
    var x = 0.0
    // Muzzle sits sightHeight BELOW the scope's line of sight. We integrate
    // in the scope frame so "y = 0" means "exactly on the line of sight".
    var y = -sightHeightM
    var vx = muzzleVelocity * cos(barrelAngleRad)
    var vy = muzzleVelocity * sin(barrelAngleRad)
    var t = 0.0
    // Sanity cap so a wild bisection guess can't hang forever.
    val maxSteps = 30_000
    var steps = 0
    while (steps++ < maxSteps) {
        if (x >= stopAtRangeM) {
            return TrajResult(true, y, t, sqrt(vx * vx + vy * vy))
        }
        // Hit the ground from below the line of sight: bullets that go
        // this far below the TARGET are physically gone. The floor has to
        // be relative to the target, not to the muzzle — a target 52 m
        // downhill is legitimately reached by passing well below the
        // muzzle's line of sight.
        if (y < abortBelowM && vy < 0.0) {
            return TrajResult(false, y, t, sqrt(vx * vx + vy * vy))
        }
        val v = sqrt(vx * vx + vy * vy)
        if (v < 1.0) return TrajResult(false, y, t, v)  // bullet stalled
        val mach = v / soundSpeed
        val cd = g1Cd(mach)
        // Drag deceleration via the G1 BC system in SI. The π/8 coefficient
        // is (1/2)·(π/4): the 1/2 comes from the kinetic-pressure formula
        // (1/2)·ρ·v², and the π/4 captures the circular cross-section of the
        // G1 reference projectile that BC is normalised against. Using 1/2
        // alone over-counts drag by ~27% and produces visibly low impact
        // velocities at known reference ranges.
        val dragAccel = (Math.PI / 8.0) * cd * airDensity * v * v / bcMetric
        val ax = -dragAccel * (vx / v)
        val ay = -dragAccel * (vy / v) - GRAVITY_MPS2
        vx += ax * INTEGRATION_DT_S
        vy += ay * INTEGRATION_DT_S
        x += vx * INTEGRATION_DT_S
        y += vy * INTEGRATION_DT_S
        t += INTEGRATION_DT_S
    }
    return TrajResult(false, y, t, sqrt(vx * vx + vy * vy))
}

private data class BarrelSolution(val barrelAngleRad: Double, val tof: Double, val impactVelocity: Double)

/**
 * Bisect on the barrel angle until the bullet passes through the target
 * point.
 *
 * The bracket is anchored to the LOOK ANGLE rather than to horizontal.
 * A fixed floor cannot express a downhill shot: to reach a target 10°
 * below the shooter the barrel itself has to sit near −10°, and any
 * bracket whose lower bound is above that converges on its own bound
 * and returns a plausible-looking but wrong angle instead of failing.
 * Anchoring to the look angle keeps the same ±width of search for a
 * level, uphill or downhill shot alike.
 */
private fun findBarrelAngle(
    targetGroundM: Double,
    targetVerticalM: Double,
    cartridge: com.example.mysnipeit.data.models.CartridgeProfile,
    sightHeightM: Double,
    airDensity: Double,
    soundSpeed: Double,
): BarrelSolution? {
    val lookAngleRad = atan2(targetVerticalM, targetGroundM)
    var lo = lookAngleRad - Math.toRadians(2.0)
    var hi = lookAngleRad + Math.toRadians(28.0)
    // The bullet may pass this far below the target before we call it lost.
    val abortBelowM = targetVerticalM - 50.0
    var last: TrajResult? = null
    for (i in 0 until 25) {  // ~10⁻⁶ rad ≈ 6 µ° — way under any UI precision
        val mid = (lo + hi) / 2
        val r = integrateTrajectory(
            barrelAngleRad = mid,
            muzzleVelocity = cartridge.muzzleVelocityMps,
            bcG1 = cartridge.ballisticCoefficientG1,
            sightHeightM = sightHeightM,
            airDensity = airDensity,
            soundSpeed = soundSpeed,
            stopAtRangeM = targetGroundM,
            abortBelowM = abortBelowM,
        )
        if (!r.reachedRange) {
            // Couldn't reach the target — push the barrel UP and retry.
            lo = mid
            continue
        }
        last = r
        if (r.dropAtRangeM < targetVerticalM) lo = mid else hi = mid
    }
    val solved = last ?: return null
    if (!solved.reachedRange) return null
    return BarrelSolution(
        barrelAngleRad = (lo + hi) / 2,
        tof = solved.tof,
        impactVelocity = solved.impactVelocity,
    )
}

// ---------------------------------------------------------------------------
// Sight angle (back-computed from the rifle's zero range)
// ---------------------------------------------------------------------------

/**
 * The angle the barrel sits ABOVE the scope's line of sight when the
 * scope is zeroed at the rifle's nominal range. Used for context only
 * (the bisection finds the absolute barrel angle directly), but kept as
 * a sanity check + future hook for "what would the come-ups be from
 * here" displays.
 *
 * Returns null if the zero range itself isn't reachable for the cartridge
 * (shouldn't happen with the catalog presets, but defensive against the
 * user dialing in absurd combinations later).
 */
internal fun sightAngleRad(
    cartridge: com.example.mysnipeit.data.models.CartridgeProfile,
    rifle: com.example.mysnipeit.data.models.RifleProfile,
    airDensity: Double,
    soundSpeed: Double,
): Double? {
    val sol = findBarrelAngle(
        targetGroundM = rifle.zeroRangeM,
        targetVerticalM = 0.0,
        cartridge = cartridge,
        sightHeightM = rifle.sightHeightMm / 1000.0,
        airDensity = airDensity,
        soundSpeed = soundSpeed,
    ) ?: return null
    return sol.barrelAngleRad
}

// ---------------------------------------------------------------------------
// Wind drift (crosswind only — full wind vector resolution is overkill for
// the POC; the headwind/tailwind correction at sub-km ranges is < 0.2°).
// ---------------------------------------------------------------------------

private fun windageDeg(
    windSpeedMps: Double?,
    windDirectionDeg: Double?,
    shotAzimuthDeg: Double,
    rangeM: Double,
    timeOfFlightS: Double,
    muzzleVelocity: Double,
): Double {
    if (windSpeedMps == null || windDirectionDeg == null) return 0.0
    // Wind direction convention: where the wind is COMING FROM. We resolve
    // it into the shooter's frame: positive sin(Δ) means wind is coming
    // from the shooter's RIGHT side, which pushes the bullet LEFT, so the
    // hold-over is to the RIGHT — which is the positive sign we want.
    //
    // Lag-time formula: hold_M ≈ v_wind_⊥ · (TOF − range / v_muzzle).
    val deltaRad = Math.toRadians(windDirectionDeg - shotAzimuthDeg)
    val crosswindFromRight = windSpeedMps * sin(deltaRad)
    val holdM = crosswindFromRight * (timeOfFlightS - rangeM / muzzleVelocity)
    return Math.toDegrees(atan2(holdM, rangeM))
}

// ---------------------------------------------------------------------------
// Confidence — UI hint, not a probability
// ---------------------------------------------------------------------------

private fun computeConfidence(windKnown: Boolean, atmosphereKnown: Boolean, range: Double): Float {
    var c = 1.0
    if (!windKnown) c *= 0.85
    if (!atmosphereKnown) c *= 0.9
    // Long-range confidence decays gently — solver itself is happy at 3 km
    // but the input uncertainty dominates the further out we go.
    if (range > 800.0) c *= (1.0 - (range - 800.0) / 4_400.0).coerceAtLeast(0.5)
    return c.toFloat()
}
