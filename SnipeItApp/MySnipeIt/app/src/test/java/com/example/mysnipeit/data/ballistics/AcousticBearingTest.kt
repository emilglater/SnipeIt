package com.example.mysnipeit.data.ballistics

import com.example.mysnipeit.data.models.AcousticEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Pure-math tests for [worldBearingFromAcousticEvent]. The second
 * argument is the operator-captured tripod-forward world bearing
 * (saved at setup time when the camera was centred at servo 90°),
 * NOT a live compass reading.
 */
class AcousticBearingTest {

    private fun event(azim: Float, valid: Boolean = true) = AcousticEvent(
        type = "acoustic_event",
        timestampUs = 0L,
        azimuthDeg = azim,
        confidence = 0.9f,
        peakAmplitude = 0.5f,
        durationMs = 4.0f,
        valid = valid,
    )

    // --- Happy path ----------------------------------------------------------

    @Test
    fun `tripod 0, event 0 gives world 0`() {
        assertEquals(0.0, worldBearingFromAcousticEvent(event(0f), 0.0)!!, 1e-9)
    }

    @Test
    fun `tripod 0, event 42 gives world 42`() {
        assertEquals(42.0, worldBearingFromAcousticEvent(event(42f), 0.0)!!, 1e-9)
    }

    @Test
    fun `tripod 90, event 30 gives world 120`() {
        assertEquals(120.0, worldBearingFromAcousticEvent(event(30f), 90.0)!!, 1e-9)
    }

    @Test
    fun `mic-frame negative azim resolves correctly`() {
        // Mic array reports −60° = "60° to the left of tripod-forward".
        // Tripod-forward at 100° → sound at 100 − 60 = 40°.
        assertEquals(40.0, worldBearingFromAcousticEvent(event(-60f), 100.0)!!, 1e-9)
    }

    // --- Wraparound past 360 / below 0 ---------------------------------------

    @Test
    fun `tripod 350 + event 20 wraps to 10`() {
        assertEquals(10.0, worldBearingFromAcousticEvent(event(20f), 350.0)!!, 1e-9)
    }

    @Test
    fun `tripod 30 + event -60 wraps to 330`() {
        // Tripod at 30° (a bit east of north) + sound 60° left = 330° (NW).
        assertEquals(330.0, worldBearingFromAcousticEvent(event(-60f), 30.0)!!, 1e-9)
    }

    @Test
    fun `tripod 359_5 + event 1 wraps to 0_5`() {
        assertEquals(0.5, worldBearingFromAcousticEvent(event(1f), 359.5)!!, 1e-5)
    }

    // --- Null guards (must never return a bearing) ---------------------------

    @Test
    fun `null event returns null`() {
        assertNull(worldBearingFromAcousticEvent(null, 100.0))
    }

    @Test
    fun `null tripod bearing means uncalibrated, returns null`() {
        // If the operator hasn't calibrated yet, the world bearing is
        // unknowable. Must never substitute 0° (true north) silently.
        assertNull(worldBearingFromAcousticEvent(event(42f), null))
    }

    @Test
    fun `valid=false on the event returns null`() {
        assertNull(worldBearingFromAcousticEvent(event(42f, valid = false), 0.0))
    }

    @Test
    fun `null event takes precedence over null tripod`() {
        assertNull(worldBearingFromAcousticEvent(null, null))
    }

    // --- Output is always normalised to [0, 360) -----------------------------

    @Test
    fun `result is always in 0 to 360 range`() {
        val cases = listOf(
            0.0 to 0f, 90.0 to 30f, 180.0 to 200f, 359.0 to 359f, 350.0 to 20f,
            0.5 to 359.9f, 1.0 to 360f,  // event=360 normalises
            45.0 to -90f, 200.0 to -200f,  // negative mic azim wraps
        )
        for ((tripod, azim) in cases) {
            val r = worldBearingFromAcousticEvent(event(azim), tripod)!!
            assert(r >= 0.0 && r < 360.0) { "out-of-range bearing $r for tripod=$tripod azim=$azim" }
        }
    }
}
