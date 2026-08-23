package com.example.mysnipeit.data.repository

import com.example.mysnipeit.data.ballistics.RigGeometry
import com.example.mysnipeit.data.models.*
import com.example.mysnipeit.data.network.RaspberryPiClient
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/**
 * Which HTTP command the app uses to ask the Pi to slew the camera to
 * an audio bearing. The Pi-side dev gets the final say on the contract;
 * the app is wired for both and switches with a one-line constant change
 * in [SniperRepository.SLEW_COMMAND_MODE].
 *
 *  - [SLEW_TO_BEARING] — app sends one intent (`{azimuth_deg: <world>}`)
 *    and the Pi handles the bearing→servo conversion + autonomous scan.
 *    RECOMMENDED: keeps geometry math on the Pi side where the
 *    autonomous-scan code already lives, and lets the Pi use the
 *    freshest compass reading at the moment of slew.
 *  - [SET_SERVO_ANGLES] — app computes servo H/V degrees from the
 *    current compass + [RigGeometry] and sends them directly. Trivial
 *    Pi-side (`servo.move(h, v)`) but couples app + Pi on the rig
 *    geometry constants, and angles can drift if the rig moves between
 *    send and receive.
 */
enum class SlewCommandMode { SLEW_TO_BEARING, SET_SERVO_ANGLES }

class SniperRepository {

    private val raspberryPiClient = RaspberryPiClient()
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    // Store current IP address for sending commands
    private var currentIpAddress: String? = null

    // Expose data streams from the network client
    val sensorData: StateFlow<SensorData?> = raspberryPiClient.sensorData
    val detectedTargets: StateFlow<List<DetectedTarget>> = raspberryPiClient.detectedTargets
    val systemStatus: StateFlow<SystemStatus> = raspberryPiClient.systemStatus
    val acousticEvent: StateFlow<AcousticEvent?> = raspberryPiClient.acousticEvent

    val streamReady: StateFlow<Boolean> = raspberryPiClient.streamReady
    val rtspStreamUrl: StateFlow<String?> = raspberryPiClient.rtspStreamUrl



    suspend fun connectToSystem(ipAddress: String) {
        currentIpAddress = ipAddress
        raspberryPiClient.connect(ipAddress)
    }

    fun disconnectFromSystem() {
        raspberryPiClient.disconnect()
        currentIpAddress = null
    }

    // Commands to send to RPi5 - FIXED to use suspend functions
    fun selectTarget(targetId: String) {
        scope.launch {
            currentIpAddress?.let { ip ->
                raspberryPiClient.sendCommand(
                    ipAddress = ip,
                    command = "select_target",
                    params = mapOf("target_id" to targetId)
                )
            }
        }
    }

    /**
     * Send lock/unlock command via WebSocket
     */
    fun sendLockCommand(targetId: String, isLocking: Boolean) {
        val action = if (isLocking) "lock" else "unlock"
        raspberryPiClient.sendLockCommand(targetId, action)
    }

    fun requestCalibration() {
        scope.launch {
            currentIpAddress?.let { ip ->
                raspberryPiClient.sendCommand(
                    ipAddress = ip,
                    command = "calibrate_system",
                    params = emptyMap()
                )
            }
        }
    }

    fun setManualTarget(latitude: Double, longitude: Double) {
        scope.launch {
            currentIpAddress?.let { ip ->
                raspberryPiClient.sendCommand(
                    ipAddress = ip,
                    command = "set_manual_target",
                    params = mapOf(
                        "latitude" to latitude,
                        "longitude" to longitude
                    )
                )
            }
        }
    }

    fun emergencyStop() {
        scope.launch {
            currentIpAddress?.let { ip ->
                raspberryPiClient.sendCommand(
                    ipAddress = ip,
                    command = "emergency_stop",
                    params = emptyMap()
                )
            }
        }
    }

    // Additional helper methods

    fun getVideoStreamUrl(): String? {
        return currentIpAddress?.let { ip ->
            raspberryPiClient.getVideoStreamUrl(ip)
        }
    }

    fun isConnected(): Boolean {
        return raspberryPiClient.isConnected()
    }

    // --- Forced mock mode (Diagnostics → MOCK MODE) -------------------------
    // Drives the offline ballistic-calculator test path. The viewmodel keeps
    // the mock anchor updated with the operator's own GPS so the mock Pi
    // stays in range of whichever city you're testing from.

    fun setMockAnchor(latDeg: Double?, lonDeg: Double?, altM: Double?) =
        raspberryPiClient.setMockAnchor(latDeg, lonDeg, altM)

    fun startForcedMock() = raspberryPiClient.startForcedMock()

    fun stopForcedMock() = raspberryPiClient.stopForcedMock()

    // --- Acoustic-alert slew -----------------------------------------------
    // Branches on SLEW_COMMAND_MODE so the Pi-side dev's choice is a
    // one-line change here. Both contracts go over the WebSocket (same
    // channel as lock/unlock) — the Pi has no HTTP server, and it parses
    // inbound command frames in its WS receive handler.

    /**
     * Ask the Pi to slew the camera toward an acoustic contact. Called
     * by [com.example.mysnipeit.viewmodel.SniperViewModel.acceptAudioAlert].
     *
     * Takes BOTH the raw mic-frame azimuth AND the world bearing so each
     * contract option uses its canonical input without any redundant
     * round-trip through the other:
     *
     *  - [SlewCommandMode.SET_SERVO_ANGLES] uses [rawMicAzimuthDeg]
     *    directly with [RigGeometry.MIC_TO_SERVO_OFFSET_DEG] — no
     *    compass involved, since the mic array and the servo are both
     *    bolted to the same fixed tripod (they share a frame, just
     *    rotated by 90°). Robust against stale/missing compass. The Pi
     *    feeds these straight to ddl_servo_set_target + a NOISE_DETECTED
     *    event, which slews the head and resumes the autonomous scan
     *    from there.
     *  - [SlewCommandMode.SLEW_TO_BEARING] uses [worldBearingDeg], so
     *    the Pi can resolve servo angles using its own freshest compass
     *    reading at the moment of slew. (Not the active contract — the
     *    Pi currently only implements set_servo_angles.)
     *
     * @param currentServoVerticalDeg the camera's CURRENT tilt, so the
     *  slew can leave it untouched. The mic array gives azimuth only, so
     *  a slew must never move the tilt; since the Pi's set_servo_angles
     *  always carries both axes, "don't move" is expressed by echoing the
     *  present angle back. Null (or an implausible reading) falls back to
     *  [RigGeometry.SERVO_VERTICAL_LEVEL_DEG].
     */
    fun slewToAcousticContact(
        rawMicAzimuthDeg: Double,
        worldBearingDeg: Double,
        currentServoVerticalDeg: Double? = null,
    ) {
        when (SLEW_COMMAND_MODE) {
            SlewCommandMode.SLEW_TO_BEARING -> {
                raspberryPiClient.sendWsCommand(
                    command = "slew_to_bearing",
                    params = mapOf("azimuth_deg" to worldBearingDeg),
                )
            }
            SlewCommandMode.SET_SERVO_ANGLES -> {
                // Direct mic-frame → servo-frame conversion. No compass,
                // no world bearing, no detour: the two frames are
                // mechanically related by a single constant offset
                // (mic 0° ↔ servo 90°). Clamp to the servo's mechanical
                // pan range (the Pi clamps again defensively).
                val servoH = (rawMicAzimuthDeg + RigGeometry.MIC_TO_SERVO_OFFSET_DEG)
                    .coerceIn(0.0, 180.0)
                // The slew is HORIZONTAL-ONLY. A 4-mic array resolves
                // azimuth, not elevation, so there is nothing to say about
                // tilt. `set_servo_angles` has no "leave this axis alone"
                // encoding, so we hold the tilt by commanding it to where it
                // already is — the operator's aim survives the slew.
                //
                // This previously sent SERVO_VERTICAL_LEVEL_DEG every time,
                // which recentred the camera to the horizon on every slew and
                // threw away the operator's elevation. "No elevation info"
                // means DON'T COMMAND elevation — not command it to level.
                //
                // ServoFrame.verticalDeg is nullable, so null already means
                // "no reading" and 0.0 means a genuine full-down aim. The range
                // check is therefore plain validation of the servo's mechanical
                // travel, not a workaround for an ambiguous default. Fall back
                // to level only when there is no reading at all.
                val servoV = currentServoVerticalDeg
                    ?.takeIf { it in 0.0..180.0 }
                    ?: RigGeometry.SERVO_VERTICAL_LEVEL_DEG
                raspberryPiClient.sendWsCommand(
                    command = "set_servo_angles",
                    params = mapOf(
                        "horizontal_deg" to servoH,
                        "vertical_deg" to servoV,
                    ),
                )
            }
        }
    }

    companion object {
        /**
         * The contract the app uses to ask the Pi to slew. **Flip this
         * one line** to switch between the two options. Picked by the
         * Pi-side dev: `SET_SERVO_ANGLES` — the app pre-computes the
         * servo angle (mic_azim + 90°, clamped to 0..180) and the Pi
         * just moves the servos. No compass involved in the slew path.
         */
        val SLEW_COMMAND_MODE: SlewCommandMode = SlewCommandMode.SET_SERVO_ANGLES
    }
}