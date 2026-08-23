package com.example.mysnipeit.data.ballistics

import com.example.mysnipeit.data.models.GpsFrame
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import kotlin.math.abs

/**
 * Pure-math tests for [localizeTarget]. Scenarios are written against
 * the actual rig mounting: [RigGeometry.COMPASS_ON_FIXED_BASE] is
 * `false` — the compass is bolted to the moving camera arm, so its
 * reading IS the camera's world-frame pointing direction and the servo
 * pan offset is NOT added to the bearing. The vertical servo angle
 * still governs the slant→horizontal split.
 *
 * Reference: 1° latitude ≈ 111.19 km at the equator-radius approximation
 * used by the localizer, so 1112 m north ≈ 0.01° lat.
 */
class TargetLocalizerTest {

    private val piGps = GpsFrame(
        valid = true,
        fixType = 3,
        numSatellites = 9,
        latitudeDeg = 31.500000,
        longitudeDeg = 34.500000,
        altitudeM = 100.0,
    )

    /** Helper: localize with rig-level servo (90/90) and a given heading. */
    private fun localize(
        heading: Float?,
        servoH: Float? = 90f,
        servoV: Float? = 90f,
        distance: Float? = 1000f,
        gps: GpsFrame? = piGps,
    ) = localizeTarget(gps, heading, servoH, servoV, distance)

    // --- Happy-path geometry -------------------------------------------------

    @Test
    fun `due north, level — target is straight up the latitude axis`() {
        val t = localize(heading = 0f)!!
        // 1000m north ≈ +0.008993° lat; lon and alt unchanged
        assertEquals(31.508993, t.latitudeDeg, 1e-4)
        assertEquals(34.500000, t.longitudeDeg, 1e-6)
        assertEquals(100.0, t.altitudeM, 1e-6)
    }

    @Test
    fun `due east, level — target is along the longitude axis only`() {
        val t = localize(heading = 90f)!!
        assertEquals(31.500000, t.latitudeDeg, 1e-6)
        // 1000m east at lat 31.5 ≈ +0.008993 / cos(31.5°) ≈ +0.010546° lon
        assertEquals(34.510546, t.longitudeDeg, 1e-4)
    }

    @Test
    fun `due south — latitude decreases`() {
        val t = localize(heading = 180f)!!
        assertEquals(31.491007, t.latitudeDeg, 1e-4)
        assertEquals(34.500000, t.longitudeDeg, 1e-6)
    }

    @Test
    fun `compass heading IS the bearing in moving-head config`() {
        // With the compass on the moving head, the heading directly tells
        // us where the camera is pointing — servo pan is NOT added on top.
        // Heading 45 → NE quadrant, regardless of any servo H value.
        val t1 = localize(heading = 45f, servoH = 90f)!!
        val t2 = localize(heading = 45f, servoH = 135f)!!
        // Same target either way (servoH ignored for bearing).
        assertEquals(t1.latitudeDeg, t2.latitudeDeg, 1e-6)
        assertEquals(t1.longitudeDeg, t2.longitudeDeg, 1e-6)
        // NE: both lat and lon increase.
        assert(t1.latitudeDeg > piGps.latitudeDeg)
        assert(t1.longitudeDeg > piGps.longitudeDeg)
        // Components ≈ 1000/√2 = 707.1m → dLat ≈ +0.006359°
        assertEquals(31.506359, t1.latitudeDeg, 1e-4)
        assertEquals(34.507457, t1.longitudeDeg, 1e-4)
    }

    @Test
    fun `heading near 360 produces north-ish bearing`() {
        // Heading 350 → 10° west of north. Both components should be
        // close to "all latitude" (lat increases) with a tiny lon dip.
        val t = localize(heading = 350f)
        assertNotNull(t)
        assert(t!!.latitudeDeg > piGps.latitudeDeg)
        assert(t.longitudeDeg < piGps.longitudeDeg)  // 10° west of N
    }

    @Test
    fun `upward tilt splits slant into shorter horizontal plus altitude gain`() {
        // 30° up (servoV 120), 1000m slant → horizontal 866m, vertical +500m
        val t = localize(heading = 0f, servoV = 120f)!!
        assertEquals(600.0, t.altitudeM, 0.5)             // 100 + 500
        assertEquals(31.507789, t.latitudeDeg, 1e-4)      // 866m north
    }

    @Test
    fun `downward tilt lowers target altitude`() {
        // 30° down (servoV 60): same horizontal, altitude 100 - 500 = -400
        val t = localize(heading = 0f, servoV = 60f)!!
        assertEquals(-400.0, t.altitudeM, 0.5)
    }

    // --- Missing-input guards (must return null, never a garbage solution) ---

    @Test
    fun `null when gps missing or invalid`() {
        assertNull(localize(heading = 0f, gps = null))
        assertNull(localize(heading = 0f, gps = piGps.copy(valid = false)))
    }

    @Test
    fun `null when compass heading missing`() {
        // The "magnetometer not fixed yet" case — heading must never be
        // silently treated as 0° (true north).
        assertNull(localize(heading = null))
    }

    @Test
    fun `servo pan NOT required in moving-head config`() {
        // With COMPASS_ON_FIXED_BASE = false, the localizer doesn't read
        // the servo pan angle at all (it's not part of the bearing math).
        // A null servoH must not block the computation.
        assertNotNull(localize(heading = 0f, servoH = null))
    }

    @Test
    fun `null when servo tilt missing`() {
        // Servo vertical IS still required — it governs the slant→
        // horizontal split that determines target altitude.
        assertNull(localize(heading = 0f, servoV = null))
    }

    @Test
    fun `null when rangefinder missing or non-positive`() {
        assertNull(localize(heading = 0f, distance = null))
        assertNull(localize(heading = 0f, distance = 0f))
        assertNull(localize(heading = 0f, distance = -5f))
    }

    // --- normalizeDeg --------------------------------------------------------

    @Test
    fun `normalizeDeg wraps both directions`() {
        assertEquals(0.0, normalizeDeg(360.0), 1e-9)
        assertEquals(35.0, normalizeDeg(395.0), 1e-9)
        assertEquals(350.0, normalizeDeg(-10.0), 1e-9)
        assertEquals(0.0, normalizeDeg(-720.0), 1e-9)
    }

    // --- Round-trip sanity ----------------------------------------------------

    @Test
    fun `distance from pi to localized target matches horizontal range`() {
        // servoH is ignored in moving-head config; left at 105 just to
        // confirm it doesn't affect the result. servoV 100 → 10° tilt up.
        val t = localize(heading = 37f, servoH = 105f, servoV = 100f, distance = 800f)!!
        val dLatM = Math.toRadians(t.latitudeDeg - piGps.latitudeDeg) * 6_371_000.0
        val dLonM = Math.toRadians(t.longitudeDeg - piGps.longitudeDeg) *
                6_371_000.0 * Math.cos(Math.toRadians(piGps.latitudeDeg))
        val groundM = Math.sqrt(dLatM * dLatM + dLonM * dLonM)
        val expected = 800.0 * Math.cos(Math.toRadians(10.0))
        assert(abs(groundM - expected) < 1.0) {
            "ground=$groundM expected=$expected"
        }
    }
}
