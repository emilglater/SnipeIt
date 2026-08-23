package com.example.mysnipeit.data.network

import android.util.Log
import com.example.mysnipeit.data.models.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.java_websocket.client.WebSocketClient
import org.java_websocket.handshake.ServerHandshake
import java.net.URI
import com.google.gson.Gson
import java.net.HttpURLConnection
import java.net.URL

/**
 * Enhanced Raspberry Pi Client
 * Handles all communication with the RPi5
 */
class RaspberryPiClient {

    companion object {
        private const val TAG = "RaspberryPiClient"
        private const val WEBSOCKET_PORT = 8555
        private const val HTTP_PORT = 8000
        private const val VIDEO_STREAM_PORT = 8554

        // Detection staleness backstop. The Pi now owns tracking and signals
        // "clear all boxes" explicitly with an empty detections array, so this
        // is ONLY a link-lost safety net: if no target_detection message has
        // arrived for STALENESS_TIMEOUT_MS the overlay is wiped. Sized for the
        // new 2-8 msg/s rate (must be longer than any normal inter-message gap
        // AND the Pi's ≤1.5 s track coast) — 3 s means "the link is dead".
        private const val STALENESS_CHECK_INTERVAL_MS = 500L
        private const val STALENESS_TIMEOUT_MS = 3000L

        // WS keepalive — prevent NAT/router idle timeouts
        private const val KEEPALIVE_INTERVAL_MS = 20_000L
    }

    private val gson = Gson()
    private var webSocketClient: WebSocketClient? = null
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    private var currentIpAddress: String? = null


    // Sensor data from RPi
    private val _sensorData = MutableStateFlow<SensorData?>(null)
    val sensorData: StateFlow<SensorData?> = _sensorData.asStateFlow()

    private val _streamReady = MutableStateFlow(false)
    val streamReady: StateFlow<Boolean> = _streamReady.asStateFlow()

    private val _rtspStreamUrl = MutableStateFlow<String?>(null)
    val rtspStreamUrl: StateFlow<String?> = _rtspStreamUrl.asStateFlow()

    // Detected targets from RPi
    private val _detectedTargets = MutableStateFlow<List<DetectedTarget>>(emptyList())
    val detectedTargets: StateFlow<List<DetectedTarget>> = _detectedTargets.asStateFlow()

    // Latest acoustic event from the Pi's 4-mic TDOA module. Single-slot
    // flow — the alert state machine (added in a later step) handles
    // de-dupe and timeout; the parser just publishes the raw event.
    private val _acousticEvent = MutableStateFlow<AcousticEvent?>(null)
    val acousticEvent: StateFlow<AcousticEvent?> = _acousticEvent.asStateFlow()

    // System status - FIXED to match your model exactly
    private val _systemStatus = MutableStateFlow(
        SystemStatus(
            connectionStatus = ConnectionState.DISCONNECTED,
            batteryLevel = null,
            cameraStatus = false,
            gpsStatus = false,
            rangefinderStatus = false,
            microphoneStatus = false,
            lastHeartbeat = System.currentTimeMillis() ,
            cpuTemperature = null,
            gpsLatitude = null,
            gpsLongitude = null
        )
    )
    val systemStatus: StateFlow<SystemStatus> = _systemStatus.asStateFlow()

    // Mock data generator
    private var mockDataJob: Job? = null
    // Synthetic acoustic-event generator, ticks on its own schedule
    // (every 20-40 s, not every 1.5 s like the sensor frames) so the
    // alert UI can be exercised end-to-end without a Pi.
    private var mockAcousticJob: Job? = null

    // --- Forced mock anchor -------------------------------------------------
    // When force-mock mode is enabled from the Diagnostics screen, the mock
    // Pi is "placed" relative to this anchor (typically the sniper's own
    // GPS) so the ballistic calculator works regardless of where the
    // operator is testing from. If the anchor is null, the generator falls
    // back to a fixed Negev test position.
    @Volatile private var mockAnchorLatDeg: Double? = null
    @Volatile private var mockAnchorLonDeg: Double? = null
    @Volatile private var mockAnchorAltM: Double? = null

    /** Update the mock Pi's anchor. Safe to call at any time. */
    fun setMockAnchor(latDeg: Double?, lonDeg: Double?, altM: Double?) {
        mockAnchorLatDeg = latDeg
        mockAnchorLonDeg = lonDeg
        mockAnchorAltM = altM
    }

    /**
     * Operator-triggered "force mock" — used by Diagnostics → MOCK MODE
     * to run the full sensor stream without a Pi. Tears down any live WS
     * first so there are no two writers racing on _sensorData.
     */
    fun startForcedMock() {
        Log.d(TAG, "Forced mock mode ENABLED")
        webSocketClient?.close()
        webSocketClient = null
        stopKeepalive()
        startMockDataGeneration()
        updateSystemStatus(ConnectionState.CONNECTED)
    }

    /** Stop the forced mock generator. Does NOT reconnect to a real Pi. */
    fun stopForcedMock() {
        Log.d(TAG, "Forced mock mode DISABLED")
        mockDataJob?.cancel()
        mockDataJob = null
        mockAcousticJob?.cancel()
        mockAcousticJob = null
        _sensorData.value = null
        _detectedTargets.value = emptyList()
        _acousticEvent.value = null
        updateSystemStatus(ConnectionState.DISCONNECTED)
    }

    // --- Detection display -------------------------------------------------
    // The Pi owns tracking now (motion-compensated, stable ids that survive
    // pans + short gaps), so the app is a pass-through: each target_detection
    // message is parsed and published verbatim, no app-side re-ID / smoothing /
    // coasting. An empty detections array clears the overlay immediately.
    @Volatile private var lastWsDetectionAt: Long = 0L
    private var stalenessJob: Job? = null

    // --- WS keepalive -------------------------------------------------------
    private var keepaliveJob: Job? = null

    init {
        startStalenessChecker()
    }

    /**
     * Connect to Raspberry Pi
     */
    suspend fun connect(ipAddress: String) = withContext(Dispatchers.IO) {
        try {
            currentIpAddress = ipAddress
            Log.d(TAG, "Attempting to connect to RPi at $ipAddress")

//            val networkTester = NetworkTester()
//            val canPing = networkTester.pingDevice(ipAddress)
//
  //          if (!canPing) {
  //              Log.e(TAG, "Cannot ping device at $ipAddress")
//                startMockDataGeneration()
//                return@withContext
//            }
//
//            Log.d(TAG, "Device is reachable")
//
 //           val portStatus = networkTester.scanPorts(
 //               ipAddress,
 //               listOf(WEBSOCKET_PORT, HTTP_PORT, VIDEO_STREAM_PORT)
 //           )
//
//            Log.d(TAG, "Port scan results: $portStatus")
//
//            if (portStatus[WEBSOCKET_PORT] == true) {
//                connectWebSocket(ipAddress)
//            } else {
//                Log.w(TAG, "WebSocket port not open, using mock data")
//                startMockDataGeneration()
//            }
//
//            if (portStatus[HTTP_PORT] == true) {
//                testHttpApi(ipAddress)
//            }
            Log.d(TAG, "Attempting direct WebSocket connection...")

            try {
                connectWebSocket(ipAddress)
                Log.d(TAG, "Successfully initiated WebSocket connection")
            } catch (e: Exception) {
                Log.e(TAG, "WebSocket connection failed: ${e.message}")
                Log.w(TAG, "Falling back to mock data")
                startMockDataGeneration()
            }

            Log.d(TAG, " Successfully connected to RPi")

        } catch (e: Exception) {
            Log.e(TAG, " Connection failed: ${e.message}", e)
            Log.d(TAG, " Falling back to mock data generation")
            startMockDataGeneration()
        }
    }

    private fun connectWebSocket(ipAddress: String) {
        val wsUri = URI("ws://$ipAddress:$WEBSOCKET_PORT")

        webSocketClient = object : WebSocketClient(wsUri) {
            override fun onOpen(handshakedata: ServerHandshake?) {
                Log.d(TAG, "WebSocket connected")
                updateSystemStatus(ConnectionState.CONNECTED)
                startKeepalive()
            }

            override fun onMessage(message: String?) {
                message?.let { handleWebSocketMessage(it) }
            }

            override fun onClose(code: Int, reason: String?, remote: Boolean) {
                Log.d(TAG, "WebSocket closed: $reason")
                stopKeepalive()
                updateSystemStatus(ConnectionState.DISCONNECTED)
                // Deliberately do NOT clear the RTSP stream state here. The WS
                // is only the control channel; the RTSP video is a separate
                // connection to the same host. A transient WS blip (which is
                // common on the flaky soft-AP link) must not tear down video —
                // the video player + stall watchdog keep the RTSP session alive
                // independently. streamReady / rtspStreamUrl are only cleared on
                // an explicit disconnect() (operator leaving the live view).
            }

            override fun onError(ex: Exception?) {
                Log.e(TAG, "WebSocket error: ${ex?.message}")
                updateSystemStatus(ConnectionState.ERROR)
            }
        }

        webSocketClient?.connect()
    }

    private fun handleWebSocketMessage(message: String) {
        try {
            Log.d(TAG, " Received: $message")

            //val dataType = gson.fromJson(message, Map::class.java)["type"] as? String
            val messageMap = gson.fromJson(message, Map::class.java)
            val dataType = messageMap["type"] as? String ?: messageMap["event"] as? String

            when (dataType) {
                "sensor_data" -> {
                    val data = gson.fromJson(message, SensorData::class.java)
                    _sensorData.value = data
                }
                "target_detection" -> {
                    // The Pi owns tracking; publish detections verbatim. `id` is
                    // a stable Pi track id — kept as-is and sent back on lock.
                    // Per-detection `confirmed` gates lockability; its ABSENCE
                    // on a detection marks the whole message as fallback /
                    // overlay-only (raw per-frame ids). An empty array clears.
                    // `timestamp_ms` is the Orin's monotonic clock — NOT epoch,
                    // NOT comparable to our clock — so we don't read it.
                    val detectionData = gson.fromJson(message, Map::class.java)
                    val detectionsArray = detectionData["detections"] as? List<Map<String, Any>>

                    if (detectionsArray != null) {
                        val targets = detectionsArray.mapNotNull { detection ->
                            try {
                                val bboxMap = detection["bbox"] as? Map<String, Any>
                                if (bboxMap != null) {
                                    val bbox = BoundingBox(
                                        x = (bboxMap["x"] as? Double)?.toInt() ?: 0,
                                        y = (bboxMap["y"] as? Double)?.toInt() ?: 0,
                                        width = (bboxMap["width"] as? Double)?.toInt() ?: 0,
                                        height = (bboxMap["height"] as? Double)?.toInt() ?: 0
                                    )
                                    val id = detection["id"] as? String ?: "UNKNOWN"
                                    val cls = detection["class"] as? String ?: "UNKNOWN"
                                    val conf = (detection["confidence"] as? Double)?.toFloat() ?: 0f
                                    // Absent → null → fallback/overlay-only, not lockable.
                                    val confirmed = detection["confirmed"] as? Boolean
                                    DetectedTarget(
                                        id = id,
                                        targetType = cls,
                                        confidence = conf,
                                        bbox = bbox,
                                        confirmed = confirmed,
                                    )
                                } else null
                            } catch (e: Exception) {
                                Log.e(TAG, "Error parsing detection: ${e.message}")
                                null
                            }
                        }
                        // Publish verbatim — no pacer/tracker. Empty list clears
                        // the overlay immediately (explicit clear from the Pi).
                        _detectedTargets.value = targets
                        lastWsDetectionAt = System.currentTimeMillis()
                        Log.d(TAG, "Detections: ${targets.size} (ids=${targets.joinToString { it.id }})")
                    }
                }
                "stream_ready" -> {
                    val streamData = gson.fromJson(message, Map::class.java)
                    val rtspPort = (streamData["rtsp_port"] as? Double)?.toInt() ?: 8554
                    val streamName = streamData["stream_name"] as? String ?: "stream"

                    // Extract IP from WebSocket connection (same IP as WebSocket)
                    val ipAddress = currentIpAddress  // You'll need to store this
                    val rtspUrl = "rtsp://$ipAddress:$rtspPort/$streamName"

                    Log.d(TAG, "Stream ready at: $rtspUrl")
                    _rtspStreamUrl.value = rtspUrl
                    _streamReady.value = true
                }
                "system_status" -> {
                    val status = gson.fromJson(message, SystemStatus::class.java)
                    _systemStatus.value = status
                }
                "acoustic_event" -> {
                    // Single TDOA detection from the Pi's mic array. The
                    // dedupe + timeout logic lives in SniperViewModel; here
                    // we just publish the raw event.
                    val event = gson.fromJson(message, AcousticEvent::class.java)
                    _acousticEvent.value = event
                    Log.d(TAG, "Acoustic event azim=${event.azimuthDeg} " +
                        "conf=${event.confidence} valid=${event.valid}")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to parse message: ${e.message}", e)
        }
    }

    private fun updateSystemStatus(connectionState: ConnectionState) {
        _systemStatus.value = _systemStatus.value.copy(
            connectionStatus = connectionState,
            lastHeartbeat = System.currentTimeMillis()
        )
    }

    private suspend fun testHttpApi(ipAddress: String): Boolean {
        return withContext(Dispatchers.IO) {
            try {
                val url = URL("http://$ipAddress:$HTTP_PORT/api/status")
                val connection = url.openConnection() as HttpURLConnection
                connection.requestMethod = "GET"
                connection.connectTimeout = 5000
                connection.readTimeout = 5000

                val responseCode = connection.responseCode
                val isSuccess = responseCode == 200

                if (isSuccess) {
                    val response = connection.inputStream.bufferedReader().readText()
                    Log.d(TAG, " HTTP API response: $response")
                } else {
                    Log.w(TAG, " HTTP API returned code: $responseCode")
                }

                connection.disconnect()
                isSuccess
            } catch (e: Exception) {
                Log.e(TAG, " HTTP API test failed: ${e.message}")
                false
            }
        }
    }

    suspend fun sendCommand(ipAddress: String, command: String, params: Map<String, Any> = emptyMap()): Boolean {
        return withContext(Dispatchers.IO) {
            try {
                val url = URL("http://$ipAddress:$HTTP_PORT/api/command")
                val connection = url.openConnection() as HttpURLConnection
                connection.requestMethod = "POST"
                connection.setRequestProperty("Content-Type", "application/json")
                connection.doOutput = true

                val jsonCommand = gson.toJson(mapOf("command" to command, "params" to params))
                connection.outputStream.write(jsonCommand.toByteArray())

                val responseCode = connection.responseCode
                val isSuccess = responseCode == 200

                Log.d(TAG, if (isSuccess) " Command sent: $command" else " Command failed: $command")

                connection.disconnect()
                isSuccess
            } catch (e: Exception) {
                Log.e(TAG, " Failed to send command: ${e.message}")
                false
            }
        }
    }

    fun getVideoStreamUrl(ipAddress: String): String {
        return "rtsp://$ipAddress:$VIDEO_STREAM_PORT/stream"
    }

    /**
     * Send a command to the Pi over the WebSocket. Envelope shape:
     * `{type:"command", command, params, timestamp}` — the Pi's WS receive
     * handler (websocket_server.c → ddl_bridge_handle_command) dispatches
     * on the `command` field.
     *
     * WS rather than HTTP because the Pi has no HTTP server (port 8000
     * isn't listening); the WS connection is already open and the Pi parses
     * inbound command frames there. This is the single command channel for
     * select_target (lock/unlock) and set_servo_angles (acoustic slew).
     * Fire-and-forget: Java-WebSocket queues to its own writer thread, so
     * this is safe to call from the main thread and returns immediately.
     */
    fun sendWsCommand(command: String, params: Map<String, Any> = emptyMap()) {
        try {
            val msg = mapOf(
                "type" to "command",
                "command" to command,
                "params" to params,
                "timestamp" to System.currentTimeMillis()
            )
            webSocketClient?.send(gson.toJson(msg))
            Log.d(TAG, "Sent WS command: $command params=$params")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to send WS command $command: ${e.message}")
        }
    }

    /**
     * Send lock/unlock command via WebSocket.
     */
    fun sendLockCommand(targetId: String, action: String) {
        // targetId is the wire track id from target_detection, verbatim. The Pi
        // reads `target_id` (or `id`) — send BOTH (same value) so it matches
        // whichever the lock-follow parser uses. Getting this field name right
        // is what makes the lock hit the chosen track instead of the Pi's
        // highest-confidence fallback.
        sendWsCommand(
            command = "select_target",
            params = mapOf(
                "target_id" to targetId,
                "id" to targetId,
                "action" to action,
            ),
        )
    }

    /**
     * Start mock data generation - FIXED with Boolean values
     */
    private fun startMockDataGeneration() {
        Log.d(TAG, " Starting mock data generation")
        startMockAcousticGeneration()

        mockDataJob?.cancel()
        mockDataJob = scope.launch {
            while (isActive) {
                // 1. Generate mock sensor data — nested structure mirroring
                //    the Pi's new ddl_frame format. Servo is omitted (it's
                //    not displayed on the dashboard and only matters once the
                //    ballistics calc is wired up).
                // Place the mock Pi ~100 m east of the anchor (typically the
                // sniper's own GPS). If no anchor is set we fall back to a
                // fixed Negev test point — the ballistic card will only
                // populate if the actual sniper GPS is within range of it.
                val anchorLat = mockAnchorLatDeg ?: 31.515
                val anchorLon = mockAnchorLonDeg ?: 34.530
                val anchorAlt = mockAnchorAltM ?: 100.0
                val cosLat = Math.cos(Math.toRadians(anchorLat))
                val piLatDeg = anchorLat
                val piLonDeg = anchorLon + Math.toDegrees(100.0 / (6_371_000.0 * cosLat))

                _sensorData.value = SensorData(
                    type = "sensor_data",
                    timestamp = System.currentTimeMillis(),
                    ddlFrame = DdlFrame(
                        distance = DistanceFrame(
                            valid = true,
                            // 300 m–600 m engagement window
                            distanceM = (300f + Math.random().toFloat() * 300f),
                            status = 0,
                            precision = 1,
                            strength = 1200,
                        ),
                        temperatureHumidity = TempHumidityFrame(
                            valid = true,
                            temperatureC = (20f + Math.random().toFloat() * 10f),
                            humidityPct = (50f + Math.random().toFloat() * 20f),
                        ),
                        // Servo MUST be non-null for the ballistic localizer
                        // to produce a result. Mostly-forward / mostly-level
                        // with small jitter so the dashboard's pointing
                        // direction changes frame-to-frame without being
                        // chaotic.
                        servo = ServoFrame(
                            horizontalDeg = 90f + (Math.random().toFloat() - 0.5f) * 30f,
                            verticalDeg = 90f + (Math.random().toFloat() - 0.5f) * 10f,
                        ),
                        gps = GpsFrame(
                            valid = true,
                            fixType = 3,
                            numSatellites = 9,
                            latitudeDeg = piLatDeg,
                            longitudeDeg = piLonDeg,
                            altitudeM = anchorAlt,
                            hAccM = 1.4,
                        ),
                        compass = CompassFrame(
                            valid = true,
                            rawX = 0,
                            rawY = 0,
                            rawZ = 0,
                            temperatureC = 22f,
                            headingDeg = Math.random().toFloat() * 360f,
                        ),
                        wind = WindFrame(
                            speedValid = true,
                            speedMps = Math.random().toFloat() * 8f,
                            directionValid = true,
                            directionDeg = Math.random().toFloat() * 360f,
                        ),
                    ),
                )

                //  2. GENERATE MOCK TARGETS — wire-style: stable numeric ids
                //  (as the Pi tracker would emit) + confirmed=true so they're
                //  lockable, exercising the post-tracker display path.
                val mockTargets = listOf(
                    DetectedTarget(
                        id = "1",
                        targetType = "HUMAN",
                        confidence = 0.85f,
                        bbox = BoundingBox(
                            x = 576,      // ~30% of 1920
                            y = 432,      // ~40% of 1080
                            width = 150,
                            height = 250
                        ),
                        confirmed = true,
                    ),
                    DetectedTarget(
                        id = "2",
                        targetType = "DRONE",
                        confidence = 0.72f,
                        bbox = BoundingBox(
                            x = 1344,     // ~70% of 1920
                            y = 540,      // ~50% of 1080
                            width = 200,
                            height = 180
                        ),
                        confirmed = true,
                    )
                )

                _detectedTargets.value = mockTargets
                Log.d(TAG, " Generated ${mockTargets.size} targets")

                // 3. Update system status
                _systemStatus.value = SystemStatus(
                    connectionStatus = ConnectionState.CONNECTED,
                    batteryLevel = 85,
                    cameraStatus = true,
                    gpsStatus = true,
                    rangefinderStatus = true,
                    microphoneStatus = true,
                    lastHeartbeat = System.currentTimeMillis()
                )

                delay(1500) // Update every 1.5 seconds
            }
        }
    }

    /**
     * Synthetic acoustic-event generator. Fires a single `valid=true`
     * AcousticEvent at a random azimuth on a 20-40 s random interval so
     * the dashboard's alert UI (dedupe, timeout, accept/dismiss, lock
     * downgrade) can be exercised offline.
     *
     * Runs alongside [startMockDataGeneration]'s sensor/target loop;
     * lifetime is tied to it via [stopForcedMock] and [disconnect].
     */
    private fun startMockAcousticGeneration() {
        mockAcousticJob?.cancel()
        mockAcousticJob = scope.launch {
            // First event waits a few seconds so the operator can see
            // the dashboard settle before the alert pops.
            delay(5_000)
            while (isActive) {
                _acousticEvent.value = AcousticEvent(
                    type = "acoustic_event",
                    timestampUs = System.nanoTime() / 1000L,
                    azimuthDeg = Math.random().toFloat() * 360f,
                    confidence = 0.6f + Math.random().toFloat() * 0.4f,
                    peakAmplitude = 0.5f + Math.random().toFloat() * 0.5f,
                    durationMs = 1f + Math.random().toFloat() * 5f,
                    valid = true,
                )
                Log.d(TAG, "Mock acoustic event emitted (azim=${_acousticEvent.value?.azimuthDeg})")
                // Random gap. 20-40s is wide enough that the operator
                // can see the alert auto-dismiss (20s) and the post-
                // dismiss debounce (30s) without events stepping on
                // each other.
                delay(20_000L + (Math.random() * 20_000L).toLong())
            }
        }
    }

    fun disconnect() {
        Log.d(TAG, "🔌 Disconnecting from RPi")

        stopKeepalive()

        webSocketClient?.close()
        webSocketClient = null

        mockDataJob?.cancel()
        mockDataJob = null
        mockAcousticJob?.cancel()
        mockAcousticJob = null

        // Reset the staleness timer so the next connection starts fresh.
        lastWsDetectionAt = 0L

        _systemStatus.value = _systemStatus.value.copy(
            connectionStatus = ConnectionState.DISCONNECTED
        )
        _sensorData.value = null
        _detectedTargets.value = emptyList()
        _acousticEvent.value = null
        // Explicit teardown (operator left the live view) — NOW clear the video
        // stream state. (A transient WS close no longer does this; see onClose.)
        _streamReady.value = false
        _rtspStreamUrl.value = null
    }

    fun isConnected(): Boolean {
        return _systemStatus.value.connectionStatus == ConnectionState.CONNECTED
    }

    // ------------------------------------------------------------------------
    // Stale detection clear — LINK-LOST BACKSTOP ONLY.
    // Normal clearing is explicit: the Pi sends an empty detections array and
    // we wipe the overlay immediately. This watchdog only fires when messages
    // stop arriving entirely for STALENESS_TIMEOUT_MS (3 s) — i.e. the link is
    // dead — so the operator doesn't stare at a frozen box. Resets after
    // clearing so it doesn't fight the mock generator (which sets
    // _detectedTargets directly without touching lastWsDetectionAt).
    // ------------------------------------------------------------------------
    private fun startStalenessChecker() {
        stalenessJob?.cancel()
        stalenessJob = scope.launch {
            while (isActive) {
                delay(STALENESS_CHECK_INTERVAL_MS)
                val last = lastWsDetectionAt
                if (last > 0 && System.currentTimeMillis() - last > STALENESS_TIMEOUT_MS) {
                    if (_detectedTargets.value.isNotEmpty()) {
                        Log.d(TAG, "No WS detections for >${STALENESS_TIMEOUT_MS}ms — clearing stale bboxes")
                        _detectedTargets.value = emptyList()
                    }
                    // Reset so we don't re-clear every tick
                    lastWsDetectionAt = 0L
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // (B) WS keepalive ping
    // Sends a small JSON ping every KEEPALIVE_INTERVAL_MS to prevent NAT or
    // router idle timeouts from killing the WS connection. Unknown message
    // types fall through silently on the C server, so no Pi change is needed.
    // ------------------------------------------------------------------------
    private fun startKeepalive() {
        stopKeepalive()
        keepaliveJob = scope.launch {
            while (isActive) {
                delay(KEEPALIVE_INTERVAL_MS)
                try {
                    val ping = mapOf(
                        "type" to "ping",
                        "timestamp" to System.currentTimeMillis()
                    )
                    webSocketClient?.send(gson.toJson(ping))
                    Log.d(TAG, "Sent WS keepalive ping")
                } catch (e: Exception) {
                    Log.w(TAG, "Keepalive ping failed: ${e.message}")
                    break
                }
            }
        }
    }

    private fun stopKeepalive() {
        keepaliveJob?.cancel()
        keepaliveJob = null
    }
}

/**
 * Expected JSON format from RPi5:
 *
 * Sensor Data — nested ddl_frame format (mirrors the Pi-side C structs):
 * {
 *   "type": "sensor_data",
 *   "timestamp": 1748600000000,
 *   "ddl_frame": {
 *     "distance":             { "valid": true, "distance_m": 420.5, "status": 0, "precision": 1, "strength": 1240 },
 *     "temperature_humidity": { "valid": true, "temperature_c": 23.5, "humidity_pct": 47.1 },
 *     "servo":                { "horizontal_deg": 87.3, "vertical_deg": 12.4 },
 *     "gps":                  { "valid": true, "fix_type": 3, "num_satellites": 9,
 *                                "latitude_deg": 32.07, "longitude_deg": 34.78,
 *                                "altitude_m": 35.2, "h_acc_m": 1.4 },
 *     "compass":              { "valid": true, "raw_x": 123, "raw_y": -45, "raw_z": 678,
 *                                "temperature_c": 22.1, "heading_deg": 187.42 },
 *     "wind":                 { "speed_valid": true, "speed_mps": 3.4,
 *                                "direction_valid": true, "direction_deg": 215.0 }
 *   }
 * }
 *
 * Display rule on the app side: each sub-frame's `valid` flag gates whether
 * the dashboard renders the values. ServoFrame has no `valid` and is treated
 * as always valid when the sub-frame is present, though its two angles are
 * individually nullable (`Float?`) so a missing field never reads as 0°.
 * WindFrame has TWO valid
 * flags — speed and direction are independent. CompassFrame's `heading_deg`
 * is emitted as JSON `null` (not a number) when the magnetometer hasn't
 * fixed yet, so the Kotlin type is `Float?`. See [SensorData] for the exact
 * Kotlin shape and the helper extensions used by the dashboard.
 *
 * Target Detection:
 * {
 *   "type": "target_detection",
 *   "timestamp_ms": 5000,
 *   "detections": [
 *     {
 *       "id": "1",
 *       "class": "HUMAN",
 *       "confidence": 0.85,
 *       "bbox": {
 *         "x": 100,
 *         "y": 50,
 *         "width": 200,
 *         "height": 400
 *       }
 *     }
 *   ]
 * }
 */