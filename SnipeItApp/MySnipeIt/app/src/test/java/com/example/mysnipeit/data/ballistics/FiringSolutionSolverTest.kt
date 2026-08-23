package com.example.mysnipeit.data.ballistics

import com.example.mysnipeit.data.models.BallisticProfiles
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

/**
 * Tests for [solveFiringSolution] and its math helpers. Reference TOF /
 * velocity numbers come from published .308 M80 / .338 Lapua trajectory
 * tables — exact match isn't expected (we use a simple G1 model + ICAO
 * atmosphere), but order-of-magnitude has to be right.
 */
class FiringSolutionSolverTest {

    private val sniperLat = 31.500000
    private val sniperLon = 34.500000
    private val sniperAlt = 0.0

    private val m80 = BallisticProfiles.cartridgeById("762x51_m80")
    private val m24 = BallisticProfiles.rifleById("m24_sws")
    private val lapua = BallisticProfiles.cartridgeById("338_lapua_mag")
    private val awm = BallisticProfiles.rifleById("awm_338")

    /** Helper: place a target N metres due north of the sniper, level. */
    private fun targetDueNorth(distanceM: Double): TargetLocation =
        TargetLocation(
            latitudeDeg = sniperLat + Math.toDegrees(distanceM / 6_371_000.0),
            longitudeDeg = sniperLon,
            altitudeM = sniperAlt,
        )

    /** Target due east at the same latitude. */
    private fun targetDueEast(distanceM: Double): TargetLocation {
        val cosLat = Math.cos(Math.toRadians(sniperLat))
        return TargetLocation(
            latitudeDeg = sniperLat,
            longitudeDeg = sniperLon + Math.toDegrees(distanceM / (6_371_000.0 * cosLat)),
            altitudeM = sniperAlt,
        )
    }

    private fun solve(
        target: TargetLocation,
        cart: com.example.mysnipeit.data.models.CartridgeProfile = m80,
        rifle: com.example.mysnipeit.data.models.RifleProfile = m24,
        windSpeed: Float? = null,
        windDir: Float? = null,
        tempC: Float? = null,
        humidity: Float? = null,
    ) = solveFiringSolution(
        sniperLatDeg = sniperLat,
        sniperLonDeg = sniperLon,
        sniperAltM = sniperAlt,
        target = target,
        cartridge = cart,
        rifle = rifle,
        windSpeedMps = windSpeed,
        windDirectionDeg = windDir,
        temperatureC = tempC,
        humidityPct = humidity,
    )

    // --- Geometry ------------------------------------------------------------

    @Test
    fun `azimuth is approximately 0 for due-north target`() {
        val s = solve(targetDueNorth(300.0))!!
        assertTrue("azimuth was ${s.azimuthDeg}", s.azimuthDeg < 1.0 || s.azimuthDeg > 359.0)
        assertEquals(300.0, s.rangeM, 0.5)
    }

    @Test
    fun `azimuth is approximately 90 for due-east target`() {
        val s = solve(targetDueEast(500.0))!!
        assertEquals(90.0, s.azimuthDeg, 0.5)
        assertEquals(500.0, s.rangeM, 0.5)
    }

    @Test
    fun `slant range exceeds ground range when target is above`() {
        val high = targetDueNorth(500.0).copy(altitudeM = sniperAlt + 100.0)
        val s = solve(high)!!
        assertEquals(500.0, s.rangeM, 1.0)
        assertTrue("slant ${s.slantRangeM} should be > ground", s.slantRangeM > s.rangeM)
        assertEquals(Math.hypot(500.0, 100.0), s.slantRangeM, 1.0)
    }

    @Test
    fun `look angle is positive uphill, negative downhill, zero on level`() {
        // 500 m horizontal, 100 m up → atan2(100, 500) ≈ 11.31°
        val up = solve(targetDueNorth(500.0).copy(altitudeM = sniperAlt + 100.0))!!
        assertEquals(11.31, up.lookAngleDeg, 0.05)
        // 500 m horizontal, 100 m down → ≈ −11.31°
        val down = solve(targetDueNorth(500.0).copy(altitudeM = sniperAlt - 100.0))!!
        assertEquals(-11.31, down.lookAngleDeg, 0.05)
        // Level shot
        val level = solve(targetDueNorth(500.0))!!
        assertEquals(0.0, level.lookAngleDeg, 0.01)
    }

    // --- Out-of-range / sanity null returns ----------------------------------

    @Test
    fun `zero distance returns null`() {
        // Target literally on the sniper — slant range below the 1 m guard.
        val same = TargetLocation(sniperLat, sniperLon, sniperAlt)
        assertNull(solve(same))
    }

    @Test
    fun `beyond max solve range returns null`() {
        val faraway = targetDueNorth(5_000.0)
        assertNull(solve(faraway))
    }

    // --- Trajectory plausibility (M80 / M24 at known ranges) -----------------

    @Test
    fun `m80 at 100m roughly matches published flight time`() {
        // .308 M80 at 100 m: ~0.13 s TOF in standard atmosphere.
        val s = solve(targetDueNorth(100.0))!!
        assertEquals(0.13, s.timeOfFlightS, 0.05)
// Impact velocity at 100 m is ~763 m/s per the reference calculator.
// The earlier 780..835 range was fitted to the pre-fix drag table's own
// output, not to an outside source, so it documented the defect.
        assertTrue("v=${s.impactVelocityMps}", s.impactVelocityMps in 740.0..790.0)
    }

    @Test
    fun `m80 at 500m roughly matches published flight time`() {
        // .308 M80 at 500 m: ~0.78 s, velocity around 500-580 m/s depending
        // on the published source (real BC is not constant with velocity;
        // our G1 model assumes it is, which inflates retained velocity a
        // touch at long range — acceptable for POC display).
        val s = solve(targetDueNorth(500.0))!!
        assertEquals(0.78, s.timeOfFlightS, 0.15)
        assertTrue("v=${s.impactVelocityMps}", s.impactVelocityMps in 480.0..620.0)
    }

    @Test
    fun `m80 at 800m roughly matches published flight time`() {
        // .308 M80 at 800 m: ~1.5 s TOF.
        val s = solve(targetDueNorth(800.0))!!
        assertEquals(1.5, s.timeOfFlightS, 0.3)
    }

    @Test
    fun `338 lapua reaches significantly further than m80`() {
        // At 1200 m, .338 LM still makes it (TOF ~ 1.7s); .308 is also in
        // range but much slower. Just sanity-check magnitudes.
        val tgt = targetDueNorth(1_200.0)
        val lapuaSol = solve(tgt, cart = lapua, rifle = awm)!!
        val m80Sol = solve(tgt)!!
        assertTrue("lapua tof ${lapuaSol.timeOfFlightS} should be < m80 tof ${m80Sol.timeOfFlightS}",
            lapuaSol.timeOfFlightS < m80Sol.timeOfFlightS)
        assertTrue("lapua impact velocity ${lapuaSol.impactVelocityMps} should exceed m80's ${m80Sol.impactVelocityMps}",
            lapuaSol.impactVelocityMps > m80Sol.impactVelocityMps)
    }

    // --- Elevation behaviour --------------------------------------------------

    @Test
    fun `elevation hold increases monotonically with range`() {
        val e100 = solve(targetDueNorth(100.0))!!.elevationDeg
        val e300 = solve(targetDueNorth(300.0))!!.elevationDeg
        val e600 = solve(targetDueNorth(600.0))!!.elevationDeg
        assertTrue("$e100 < $e300", e100 < e300)
        assertTrue("$e300 < $e600", e300 < e600)
    }

    @Test
    fun `elevation hold near zero when shooting at the zero range`() {
        // M24/M80 is zeroed at 100m. Hold-over above target should be near 0.
        val s = solve(targetDueNorth(100.0))!!
        assertTrue("hold-over ${s.elevationDeg}° should be tiny near zero",
            abs(s.elevationDeg) < 0.05)
    }

    @Test
    fun `rifleman's rule — same slant range, uphill needs less hold-over`() {
        // Canonical "rifleman's rule": gravity only acts vertically, so the
        // drop a bullet accumulates is governed by the HORIZONTAL leg of the
        // flight. A 500 m slant shot inclined 20° has only ~470 m horizontal,
        // so it needs less hold-over than a flat 500 m shot.
        val flat = solve(targetDueNorth(500.0))!!.elevationDeg
        val slant = 500.0
        val angleRad = Math.toRadians(20.0)
        val horiz = slant * Math.cos(angleRad)
        val vert = slant * Math.sin(angleRad)
        val inclined = solve(targetDueNorth(horiz).copy(altitudeM = sniperAlt + vert))!!.elevationDeg
        assertTrue("inclined hold $inclined should be less than flat hold $flat", inclined < flat)
    }

    // --- Downhill shots -------------------------------------------------------
    // Regression cover for a defect found while building the Chapter 11
    // sensitivity table: the barrel-angle bisection used to search a fixed
    // [−2°, +28°] bracket. A target more than ~2° below the shooter needs a
    // barrel angle outside that bracket, so the search saturated on its own
    // lower bound and returned a wrong hold-over WITHOUT reporting failure.
    // The existing look-angle test did not catch it because the look angle is
    // computed before the bisection runs.

    @Test
    fun `downhill shot holds near the level-shot value, not many degrees`() {
        // 300 m slant, 10° down → 295.4 m horizontal, 52.1 m below.
        val slant = 300.0
        val angleRad = Math.toRadians(10.0)
        val horiz = slant * Math.cos(angleRad)
        val vert = slant * Math.sin(angleRad)
        val down = solve(targetDueNorth(horiz).copy(altitudeM = sniperAlt - vert))
        assertNotNull("downhill target should be solvable", down)
        val hold = down!!.elevationDeg
        // Before the fix this returned ≈ 7.9°. Hold-over at this range is a
        // fraction of a degree whether the shot is level, uphill or downhill.
        assertTrue("downhill hold-over $hold is implausibly large", abs(hold) < 0.5)
    }

    @Test
    fun `steep downhill beyond the old bracket still solves`() {
        // 500 m horizontal, 100 m below → look angle ≈ −11.3°, well outside
        // the old lower bound, and 100 m below the old absolute −50 m floor.
        val down = solve(targetDueNorth(500.0).copy(altitudeM = sniperAlt - 100.0))
        assertNotNull("steep downhill target should be solvable", down)
        val hold = down!!.elevationDeg
        assertTrue("steep downhill hold-over $hold is implausibly large", abs(hold) < 1.0)
    }

    @Test
    fun `uphill and downhill at equal inclination need comparable hold-over`() {
        val slant = 400.0
        val angleRad = Math.toRadians(15.0)
        val horiz = slant * Math.cos(angleRad)
        val vert = slant * Math.sin(angleRad)
        val up = solve(targetDueNorth(horiz).copy(altitudeM = sniperAlt + vert))!!.elevationDeg
        val down = solve(targetDueNorth(horiz).copy(altitudeM = sniperAlt - vert))!!.elevationDeg
        // Gravity acts vertically, so what governs the hold is the horizontal
        // leg, which is identical here. The two must be close; they are not
        // required to be exactly equal.
        assertEquals("uphill $up and downhill $down should be comparable", up, down, 0.15)
    }

    // --- Wind drift -----------------------------------------------------------

    @Test
    fun `no wind input means zero windage`() {
        val s = solve(targetDueNorth(500.0))!!
        assertEquals(0.0, s.windageDeg, 1e-9)
    }

    @Test
    fun `headwind alone produces near-zero windage`() {
        // Shot due north, wind FROM due north (head-on): crosswind = 0.
        val s = solve(targetDueNorth(500.0), windSpeed = 10f, windDir = 0f)!!
        assertEquals(0.0, s.windageDeg, 0.02)
    }

    @Test
    fun `wind from the right pushes the bullet left, so aim right`() {
        // Shot azimuth 0 (north). Wind FROM east (90°): blows the bullet to
        // the WEST. The aim correction must be to the EAST (positive deg).
        val s = solve(targetDueNorth(500.0), windSpeed = 5f, windDir = 90f)!!
        assertTrue("windage ${s.windageDeg} should be > 0", s.windageDeg > 0.0)
    }

    @Test
    fun `wind from the left flips the windage sign`() {
        val right = solve(targetDueNorth(500.0), windSpeed = 5f, windDir = 90f)!!.windageDeg
        val left = solve(targetDueNorth(500.0), windSpeed = 5f, windDir = 270f)!!.windageDeg
        assertEquals(right, -left, 0.01)
    }

    // --- Confidence -----------------------------------------------------------

    @Test
    fun `confidence is maxed when all inputs known`() {
        val s = solve(
            targetDueNorth(300.0),
            windSpeed = 2f, windDir = 90f,
            tempC = 20f, humidity = 50f,
        )!!
        assertEquals(1.0f, s.confidence, 1e-6f)
    }

    @Test
    fun `confidence drops when wind missing`() {
        val s = solve(targetDueNorth(300.0))!!
        assertTrue("confidence ${s.confidence} should be < 1", s.confidence < 1.0f)
    }

    // --- Math helpers ---------------------------------------------------------

    @Test
    fun `air density at ICAO sea level matches the textbook 1_225 value`() {
        val rho = airDensityKgPerM3(tempC = 15.0, humidityPct = 0.0, altitudeM = 0.0)
        assertEquals(1.225, rho, 0.005)
    }

    @Test
    fun `humid air is less dense than dry air at the same temperature`() {
        val dry = airDensityKgPerM3(tempC = 25.0, humidityPct = 0.0, altitudeM = 0.0)
        val humid = airDensityKgPerM3(tempC = 25.0, humidityPct = 80.0, altitudeM = 0.0)
        assertTrue("humid $humid should be < dry $dry", humid < dry)
    }

    @Test
    fun `sound speed at 15C matches the textbook 340 mps`() {
        assertEquals(340.0, soundSpeedMps(15.0), 1.0)
    }

    @Test
    fun `g1 drag coefficient peaks just after Mach 1`() {
        // The G1 curve has its global max in the ~Mach 1.4-1.6 plateau.
        val belowSonic = g1Cd(0.5)
        val transonic = g1Cd(1.5)
        val highSupersonic = g1Cd(3.5)
        assertTrue(transonic > belowSonic)
        assertTrue(transonic > highSupersonic)
    }

    @Test
    fun `g1 drag clamps outside the table`() {
        // Below 0 and above 5.0 should return the endpoint values exactly.
        assertEquals(g1Cd(0.0), g1Cd(-5.0), 1e-9)
        assertEquals(g1Cd(5.0), g1Cd(10.0), 1e-9)
    }

    @Test
    fun `sight angle is positive for a standard 100m zero`() {
        // The barrel must point slightly above the line of sight for the
        // bullet to arc back through it at 100 m.
        val angle = sightAngleRad(
            cartridge = m80,
            rifle = m24,
            airDensity = 1.225,
            soundSpeed = 340.0,
        )!!
        assertTrue("sight angle ${Math.toDegrees(angle)}° should be > 0", angle > 0.0)
        assertTrue("sight angle ${Math.toDegrees(angle)}° unreasonably large",
            Math.toDegrees(angle) < 1.0)
    }
}
