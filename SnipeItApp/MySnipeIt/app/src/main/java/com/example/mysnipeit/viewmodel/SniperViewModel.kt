package com.example.mysnipeit.viewmodel

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.example.mysnipeit.data.ballistics.FiringSolution
import com.example.mysnipeit.data.ballistics.localizeTarget
import com.example.mysnipeit.data.ballistics.solveFiringSolution
import com.example.mysnipeit.data.ballistics.worldBearingFromAcousticEvent
import com.example.mysnipeit.data.location.DeviceLocationProvider
import com.example.mysnipeit.data.models.*
import com.example.mysnipeit.data.network.WifiBinder
import com.example.mysnipeit.data.repository.SniperRepository
import com.google.android.gms.maps.model.LatLng
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import android.util.Log

class SniperViewModel(application: Application) : AndroidViewModel(application) {

    private val repository = SniperRepository()

    private val _uiState = MutableStateFlow(SniperUiState())

    val streamReady: StateFlow<Boolean> = repository.streamReady
    val rtspStreamUrl: StateFlow<String?> = repository.rtspStreamUrl


    val uiState: StateFlow<SniperUiState> = _uiState.asStateFlow()

    // --- Theme mode (dark / light) -----------------------------------------
    // Persisted to SharedPreferences so the choice survives app restarts.
    private val prefs = application.getSharedPreferences("snipeit", android.content.Context.MODE_PRIVATE)
    private val _darkTheme = MutableStateFlow(prefs.getBoolean("dark_theme", true))
    val darkTheme: StateFlow<Boolean> = _darkTheme.asStateFlow()
    fun toggleTheme() {
        val next = !_darkTheme.value
        _darkTheme.value = next
        prefs.edit().putBoolean("dark_theme", next).apply()
    }

    // --- Ballistic loadout (cartridge + rifle profile) ----------------------
    // Persisted to SharedPreferences like dark_theme so the operator's pick
    // survives restarts. Unknown/absent ids fall back to the catalog default
    // via cartridgeById/rifleById. The ballistic solver reads these flows.
    private val _selectedCartridge = MutableStateFlow(
        BallisticProfiles.cartridgeById(prefs.getString("cartridge_id", null))
    )
    val selectedCartridge: StateFlow<CartridgeProfile> = _selectedCartridge.asStateFlow()

    private val _selectedRifle = MutableStateFlow(
        BallisticProfiles.rifleById(prefs.getString("rifle_id", null))
    )
    val selectedRifle: StateFlow<RifleProfile> = _selectedRifle.asStateFlow()

    fun selectCartridge(id: String) {
        _selectedCartridge.value = BallisticProfiles.cartridgeById(id)
        prefs.edit().putString("cartridge_id", id).apply()
    }

    fun selectRifle(id: String) {
        _selectedRifle.value = BallisticProfiles.rifleById(id)
        prefs.edit().putString("rifle_id", id).apply()
    }

    // --- Tripod world bearing (operator-calibrated) -------------------------
    // Captured once at setup time when the operator centres the camera at
    // servo 90° (tripod-forward) and taps MENU → Calibrate Bearing. At that
    // moment, the compass — which is on the moving head, not the fixed
    // tripod — reads the world bearing of the tripod-forward direction,
    // which is also the world bearing of the mic array's 0° axis. This
    // captured value is what acoustic events get added to.
    //
    // Persisted to SharedPreferences (Float) so it survives app restart;
    // operator re-calibrates after moving the tripod.
    private val _tripodWorldBearingDeg = MutableStateFlow<Double?>(
        if (prefs.contains("tripod_world_bearing_deg"))
            prefs.getFloat("tripod_world_bearing_deg", 0f).toDouble()
        else null
    )
    val tripodWorldBearingDeg: StateFlow<Double?> = _tripodWorldBearingDeg.asStateFlow()

    /**
     * Wall-clock time of the most recent successful calibration. Lets the
     * UI render a "last calibrated: X min ago" hint without expiring the
     * calibration itself — option (B) from the design conversation.
     * Persisted alongside the value as `tripod_calibrated_at_ms`.
     */
    private val _tripodCalibratedAtMs = MutableStateFlow<Long?>(
        if (prefs.contains("tripod_calibrated_at_ms"))
            prefs.getLong("tripod_calibrated_at_ms", 0L).takeIf { it > 0L }
        else null
    )
    val tripodCalibratedAtMs: StateFlow<Long?> = _tripodCalibratedAtMs.asStateFlow()

    /**
     * How long a calibration stays valid before it's treated as expired.
     * Past this age the saved world bearing is no longer trusted (the
     * operator has probably moved the tripod, or it's just gone stale),
     * so acoustic alerts fall back to showing the RELATIVE mic angle
     * instead of a world bearing. The saved value isn't wiped — it stays
     * for reference in the calibrate dialog — its USE is just gated.
     *
     * CHANGE THIS to tune the expiry window. Matches the dashboard
     * CalibrationAgeChip's red band, so "chip turns red" == "expired".
     */
    val tripodCalibrationTimeoutMs: Long = 90L * 60_000L

    /** True when a calibration exists AND hasn't aged past the timeout. */
    fun isCalibrationValid(nowMs: Long = System.currentTimeMillis()): Boolean {
        val at = _tripodCalibratedAtMs.value ?: return false
        return (nowMs - at) < tripodCalibrationTimeoutMs
    }

    /** The world bearing to USE right now: the saved value if the
     *  calibration is still valid, else null (expired or never set). */
    private fun effectiveTripodWorldBearingDeg(nowMs: Long): Double? =
        if (isCalibrationValid(nowMs)) _tripodWorldBearingDeg.value else null

    /**
     * Capture the current (latched) compass heading and save it as the
     * tripod-forward world bearing. Caller is responsible for having the
     * camera centred at servo 90° / pointing at tripod-forward; the
     * compass at that moment IS the bearing we want to store.
     *
     * Returns the captured value, or null when no compass fix is
     * available — the UI shows "no fix" rather than silently saving
     * garbage.
     */
    fun calibrateTripodWorldBearing(): Double? {
        val compass = latchedSensorData.value.compassHeadingDeg()?.toDouble()
            ?: return null
        val now = System.currentTimeMillis()
        _tripodWorldBearingDeg.value = compass
        _tripodCalibratedAtMs.value = now
        prefs.edit()
            .putFloat("tripod_world_bearing_deg", compass.toFloat())
            .putLong("tripod_calibrated_at_ms", now)
            .apply()
        Log.d("SniperViewModel", "Tripod world bearing calibrated to $compass°")
        return compass
    }

    // --- Device GPS --------------------------------------------------------
    // Real device location, populated once the runtime location permission
    // has been granted (see MainActivity). Null until then OR while we wait
    // for the first fix. Used by:
    //  - MapScreen for the "Your Location" marker
    //  - (future) Ballistics calculator as one of its inputs
    private val locationProvider = DeviceLocationProvider(application.applicationContext)
    val userLocation: StateFlow<LatLng?> = locationProvider.location
    val userAltitudeM: StateFlow<Double?> = locationProvider.altitudeM

    /** Called by MainActivity after the user grants ACCESS_FINE_LOCATION. */
    fun onLocationPermissionGranted() {
        locationProvider.start()
        Log.d("SniperViewModel", "Location permission granted — provider started")
    }

    /** Called by MainActivity if the user denies the permission. */
    fun onLocationPermissionDenied() {
        Log.w("SniperViewModel", "Location permission denied — userLocation will stay null")
    }

    // --- Forced mock mode (Diagnostics → MOCK MODE) -------------------------
    // Lets the operator test the ballistic calculator end-to-end without a
    // Pi. The mock Pi's anchor is kept in sync with the operator's own GPS
    // so the calculator stays in range regardless of where the device is.
    private val _forceMockMode = MutableStateFlow(false)
    val forceMockMode: StateFlow<Boolean> = _forceMockMode.asStateFlow()

    // --- RTSP transport (debug) --------------------------------------------
    // TCP (default) matches the Pi's `-rtsp_transport tcp` and is reliable on
    // a clean link but stalls-and-wedges on a lossy one (a lost packet blocks
    // the whole stream until retransmit). UDP degrades gracefully instead —
    // brief visual artifacts, instant recovery — which is often better on the
    // flaky soft-AP link. Exposed as a Diagnostics toggle for A/B testing with
    // the Pi side (mediaMTX serves both). Persisted so the choice survives a
    // restart. Requires the Pi to allow UDP transport for the UDP setting to
    // actually stream.
    private val _rtspForceTcp = MutableStateFlow(prefs.getBoolean("rtsp_force_tcp", true))
    val rtspForceTcp: StateFlow<Boolean> = _rtspForceTcp.asStateFlow()
    fun setRtspForceTcp(forceTcp: Boolean) {
        _rtspForceTcp.value = forceTcp
        prefs.edit().putBoolean("rtsp_force_tcp", forceTcp).apply()
    }

    init {
        // Push the operator's latest GPS into the repository so the mock
        // generator always picks up a fresh anchor on its next tick.
        viewModelScope.launch {
            combine(userLocation, userAltitudeM) { latLng, alt -> latLng to alt }
                .collect { (latLng, alt) ->
                    repository.setMockAnchor(latLng?.latitude, latLng?.longitude, alt)
                }
        }
    }

    fun setForceMockMode(enabled: Boolean) {
        if (_forceMockMode.value == enabled) return
        _forceMockMode.value = enabled
        if (enabled) {
            // Tear down any real Pi connection — mock writes would race
            // with WS writes otherwise — and start the mock generator.
            WifiBinder.release(getApplication<Application>().applicationContext)
            repository.startForcedMock()
        } else {
            repository.stopForcedMock()
        }
    }

    // All 4 devices
    private val _availableDevices = MutableStateFlow(
        listOf(
            Device(
                id = "device_1",
                name = "Device 1",
                location = "Sector A",
                longitude = 34.437713,
                latitude = 31.467357,
                status = DeviceStatus.INACTIVE,
                batteryLevel = 89,
                ipAddress = "192.168.1.100"
            ),
            Device(
                id = "device_2",
                name = "Device 2",
                location = "Sector B",
                longitude = 34.484580,
                latitude = 31.527528,
                status = DeviceStatus.INACTIVE,
                batteryLevel = 72,
                ipAddress = "192.168.1.101"
            ),
            Device(
                id = "device_3",
                name = "Device 3",
                location = "Sector C",
                longitude = 34.492314,
                latitude = 31.513963,
                status = DeviceStatus.ACTIVE,
                batteryLevel = 95,
                ipAddress = WifiBinder.FALLBACK_GATEWAY  // RPi5 AP gateway (10.42.1.1)
            ),
            Device(
                id = "device_4",
                name = "Device 4",
                location = "Sector D",
                longitude = 34.452488,
                latitude = 31.514924,
                status = DeviceStatus.INACTIVE,
                batteryLevel = 87,
                ipAddress = "192.168.1.104"
            )
        )
    )
    val availableDevices: StateFlow<List<Device>> = _availableDevices

    private val _selectedDevice = MutableStateFlow<Device?>(null)
    val selectedDevice: StateFlow<Device?> = _selectedDevice

    // Data from repository
    val sensorData: StateFlow<SensorData?> = repository.sensorData
    val detectedTargets: StateFlow<List<DetectedTarget>> = repository.detectedTargets
    val systemStatus: StateFlow<SystemStatus> = repository.systemStatus
    // Raw acoustic events from the Pi's 4-mic TDOA module. The dashboard
    // alert UI (added in a later commit) consumes a derived flow that
    // applies dedupe + auto-dismiss; this raw flow is what the Diagnostics
    // LIVE SENSORS pane shows so the operator can see actual flag state.
    val acousticEvent: StateFlow<AcousticEvent?> = repository.acousticEvent

    // --- Sensor latching + history -----------------------------------------
    // When the Pi reports a sub-frame with valid=false, the dashboard would
    // previously snap to "—". Operators asked for stickier behaviour: keep
    // showing the last GOOD reading for a short grace window, then fall back
    // to "—" only if the sensor stays invalid long enough that the cached
    // value is no longer trustworthy.
    //
    // CHANGE THIS to tune how long a stale reading is held before the UI
    // shows "—". Frames typically arrive at 1.5 s in mock mode (one Pi
    // dispatch per cycle in startMockDataGeneration); the real Pi rate is
    // whatever its firmware emits. 5 s ≈ ~3 missed mock frames.
    val sensorLatchTimeoutMs: Long = 5_000L

    private data class Latched<T>(val value: T, val timestamp: Long)
    private var distanceLatch: Latched<DistanceFrame>? = null
    private var tempHumLatch: Latched<TempHumidityFrame>? = null
    private var servoLatch: Latched<ServoFrame>? = null
    private var gpsLatch: Latched<GpsFrame>? = null
    private var compassLatch: Latched<CompassFrame>? = null
    // Wind speed and direction latch INDEPENDENTLY because the Pi has two
    // separate valid flags (one channel can glitch while the other reads).
    private var windSpeedLatch: Latched<Float>? = null
    private var windDirectionLatch: Latched<Float>? = null

    private val _latchedSensorData = MutableStateFlow<SensorData?>(null)
    /**
     * SensorData where each sub-frame is the most recent VALID reading,
     * held for up to [sensorLatchTimeoutMs]. Consumed by the dashboard so a
     * momentary `valid=false` glitch doesn't blank a cell. Diagnostics
     * reads the raw [sensorData] so the operator can still see actual flag
     * state straight from the Pi.
     */
    val latchedSensorData: StateFlow<SensorData?> = _latchedSensorData.asStateFlow()

    private val _sensorHistory = MutableStateFlow<List<SensorData>>(emptyList())
    /**
     * Rolling window of the last [SENSOR_HISTORY_SIZE] raw sensor frames
     * (oldest first). Powers the Diagnostics → LIVE SENSORS panel. Updated
     * only when a NEW frame arrives — not on the periodic timeout sweep.
     */
    val sensorHistory: StateFlow<List<SensorData>> = _sensorHistory.asStateFlow()

    /**
     * App-computed firing solution. Recomputes any time the latched sensor
     * stream changes, the sniper's GPS updates, or the operator swaps the
     * cartridge / rifle profile. Null when there aren't enough inputs to
     * localise the target (e.g. compass hasn't fixed) or when the target is
     * out of range for the loadout.
     *
     * The two-piece pipeline:
     *   latched Pi sensors  ──►  localizeTarget  ──► target world coords
     *   sniper GPS + loadout + atmosphere ──► solveFiringSolution
     *
     * The Pi does NOT compute or send a firing solution — by design it
     * only publishes the raw sensor data needed to compute one. It has to
     * be this way: the solution depends on where the SHOOTER stands, and
     * the rig doesn't know that. This flow is the only firing solution
     * that exists in the system.
     */
    val firingSolution: StateFlow<FiringSolution?> = combine(
        latchedSensorData,
        userLocation,
        userAltitudeM,
        selectedCartridge,
        selectedRifle,
    ) { sensor, sniperLatLng, sniperAlt, cart, rifle ->
        computeFiringSolution(sensor, sniperLatLng, sniperAlt, cart, rifle)
    }.stateIn(viewModelScope, SharingStarted.Eagerly, null)

    private fun computeFiringSolution(
        sensor: SensorData?,
        sniperLatLng: LatLng?,
        sniperAltM: Double?,
        cart: CartridgeProfile,
        rifle: RifleProfile,
    ): FiringSolution? {
        if (sniperLatLng == null) return null
        // Pi POV → target world coordinates. Returns null if compass / GPS /
        // rangefinder aren't all available — that's the right behaviour;
        // a missing input must not become a 0° silent substitution.
        val target = localizeTarget(
            piGps = sensor?.ddlFrame?.gps,
            compassHeadingDeg = sensor.compassHeadingDeg(),
            servoHorizontalDeg = sensor.servoHorizontalDeg(),
            servoVerticalDeg = sensor.servoVerticalDeg(),
            rangefinderDistanceM = sensor.distanceM(),
        ) ?: return null
        // Sniper POV → firing solution. Fall back to the target's altitude
        // when the tablet's GPS has no vertical fix (treats the shot as
        // level, which is the least-bad assumption short of guessing).
        val tempHum = sensor?.ddlFrame?.temperatureHumidity?.takeIf { it.valid }
        return solveFiringSolution(
            sniperLatDeg = sniperLatLng.latitude,
            sniperLonDeg = sniperLatLng.longitude,
            sniperAltM = sniperAltM ?: target.altitudeM,
            target = target,
            cartridge = cart,
            rifle = rifle,
            windSpeedMps = sensor.windSpeedMps(),
            windDirectionDeg = sensor.windDirectionDeg(),
            temperatureC = tempHum?.temperatureC,
            humidityPct = tempHum?.humidityPct,
        )
    }

    init {
        // Build the latched stream on every new raw frame so the UI gets
        // an immediate update without waiting for the next 500ms tick.
        viewModelScope.launch {
            repository.sensorData.collect { frame ->
                if (frame != null) {
                    val history = _sensorHistory.value + frame
                    _sensorHistory.value = if (history.size > SENSOR_HISTORY_SIZE)
                        history.takeLast(SENSOR_HISTORY_SIZE) else history
                }
                applyLatchAndEmit()
            }
        }
        // Periodic sweep enforces the timeout even when the Pi has gone
        // silent OR keeps re-sending the same valid=false frame.
        viewModelScope.launch {
            while (isActive) {
                delay(500)
                applyLatchAndEmit()
            }
        }
    }

    private fun applyLatchAndEmit() {
        val raw = repository.sensorData.value
        val ddl = raw?.ddlFrame
        val now = System.currentTimeMillis()

        // Refresh each latch from the current frame ----------------------
        ddl?.distance?.let { if (it.valid) distanceLatch = Latched(it, now) }
        ddl?.temperatureHumidity?.let { if (it.valid) tempHumLatch = Latched(it, now) }
        // Servo has no valid flag, so presence is the only signal — but the
        // two angles are nullable, so only latch a frame that actually carries
        // them. Without this a partial frame would displace the last good one,
        // and the acoustic slew (which holds the tilt by echoing the current
        // verticalDeg back) would fall through to level and recentre the
        // camera — the exact bug the nullable angles exist to prevent. Same
        // rule as the compass below.
        ddl?.servo?.let {
            if (it.horizontalDeg != null && it.verticalDeg != null) {
                servoLatch = Latched(it, now)
            }
        }
        ddl?.gps?.let { if (it.valid) gpsLatch = Latched(it, now) }
        // Compass only latches when both `valid` AND headingDeg are non-null
        // (matches the rule in compassHeadingDeg() — a missing heading must
        // never be silently substituted as 0° / true north).
        ddl?.compass?.let { if (it.valid && it.headingDeg != null) compassLatch = Latched(it, now) }
        ddl?.wind?.let {
            if (it.speedValid) windSpeedLatch = Latched(it.speedMps, now)
            if (it.directionValid) windDirectionLatch = Latched(it.directionDeg, now)
        }

        // Expire stale latches ------------------------------------------
        if (distanceLatch?.let { now - it.timestamp > sensorLatchTimeoutMs } == true) distanceLatch = null
        if (tempHumLatch?.let { now - it.timestamp > sensorLatchTimeoutMs } == true) tempHumLatch = null
        if (servoLatch?.let { now - it.timestamp > sensorLatchTimeoutMs } == true) servoLatch = null
        if (gpsLatch?.let { now - it.timestamp > sensorLatchTimeoutMs } == true) gpsLatch = null
        if (compassLatch?.let { now - it.timestamp > sensorLatchTimeoutMs } == true) compassLatch = null
        if (windSpeedLatch?.let { now - it.timestamp > sensorLatchTimeoutMs } == true) windSpeedLatch = null
        if (windDirectionLatch?.let { now - it.timestamp > sensorLatchTimeoutMs } == true) windDirectionLatch = null

        // Compose the latched SensorData. Sub-frames carry valid=true when
        // a latched value exists, so the helper extensions in SensorData.kt
        // don't need to change.
        val latchedWind = if (windSpeedLatch != null || windDirectionLatch != null) {
            WindFrame(
                speedValid = windSpeedLatch != null,
                speedMps = windSpeedLatch?.value ?: 0f,
                directionValid = windDirectionLatch != null,
                directionDeg = windDirectionLatch?.value ?: 0f,
            )
        } else null

        _latchedSensorData.value = SensorData(
            type = raw?.type ?: "sensor_data",
            timestamp = raw?.timestamp ?: now,
            ddlFrame = DdlFrame(
                distance = distanceLatch?.value,
                temperatureHumidity = tempHumLatch?.value,
                servo = servoLatch?.value,
                gps = gpsLatch?.value,
                compass = compassLatch?.value,
                wind = latchedWind,
            ),
        )
    }

    // --- Acoustic alert state machine ---------------------------------------
    // Transforms the raw `acousticEvent` stream into a derived
    // `activeAudioAlert` that:
    //   - converts the mic-frame azimuth into a world bearing (compass +
    //     RigGeometry.MIC_ARRAY_OFFSET_DEG via worldBearingFromAcousticEvent);
    //   - dedupes same-source events within audioAlertDedupeWindowDeg (refreshes
    //     the existing alert in place instead of replacing it with a new card);
    //   - auto-dismisses an alert after audioAlertTimeoutMs from FIRST detection
    //     (timeout is NOT refreshed on dedupe — a continuously-firing source
    //     still clears itself eventually so the HUD doesn't accumulate stale
    //     alerts);
    //   - suppresses re-firing within audioAlertDedupeWindowDeg of a recently
    //     dismissed bearing for dismissDebounceMs, so an explicit DISMISS isn't
    //     instantly overruled by the next event from the same source;
    //   - flips isInteractive based on whether a target is currently selected
    //     (locked engagement → passive chip; deselected → full card with
    //     buttons), and flips back automatically when the operator deselects.

    /** Auto-dismiss timeout. Tunable — same pattern as sensorLatchTimeoutMs.
     *  Counted from the FIRST event of a dedupe group, not the latest. */
    val audioAlertTimeoutMs: Long = 20_000L

    /** Two events within this angular distance (shortest path on the circle)
     *  are treated as the same source — refresh the existing alert instead of
     *  replacing it, and a recent DISMISS suppresses re-fires within this same
     *  window. ±15° is roughly twice the typical TDOA error of a 4-mic array
     *  at sub-100 m ranges. */
    private val audioAlertDedupeWindowDeg: Double = 15.0

    /** How long an explicit DISMISS suppresses re-firing for the same source. */
    private val dismissDebounceMs: Long = 30_000L

    /** How long the "SLEWING TO X°…" notice stays on screen after the
     *  operator hits SLEW, before the alert is swept away. Long enough
     *  to confirm the tap registered; short enough not to clutter. */
    private val slewNoticeMs: Long = 1_500L

    private val _activeAudioAlert = MutableStateFlow<AudioAlert?>(null)
    /**
     * Derived alert with dedupe + auto-dismiss + lock-aware render mode.
     * The dashboard reads this for the corner card / passive chip. Null
     * when no current alert (no event yet, dismissed, or timed out).
     */
    val activeAudioAlert: StateFlow<AudioAlert?> = _activeAudioAlert.asStateFlow()

    // Raw mic azimuth of the last explicitly-dismissed source (the dedupe
    // key), used to suppress re-fires for dismissDebounceMs.
    private var lastDismissedBearing: Double? = null
    private var lastDismissedAtMs: Long = 0L

    init {
        // React to new raw events from the Pi.
        viewModelScope.launch {
            repository.acousticEvent.collect { applyAcousticEvent(it) }
        }
        // React to lock-state changes — flip isInteractive without
        // mutating any other field of the alert.
        viewModelScope.launch {
            uiState.collect { state -> updateAlertInteractivity(state.selectedTargetId) }
        }
        // Periodic timeout sweep.
        viewModelScope.launch {
            while (isActive) {
                delay(500)
                sweepAlertTimeout()
            }
        }
    }

    private fun applyAcousticEvent(event: AcousticEvent?) {
        // Need a VALID event to do anything; an invalid TDOA solve isn't
        // actionable. (The Diagnostics LIVE SENSORS pane still shows the
        // raw event regardless, so the operator sees events are arriving.)
        if (event == null || !event.valid) return
        val now = System.currentTimeMillis()

        // World bearing only when the calibration is present AND unexpired.
        // Otherwise the alert still fires (Option B) but shows the RELATIVE
        // mic angle — the SLEW action works either way since it only needs
        // the raw azimuth (mic_azim + 90 → servo).
        val worldBearing = worldBearingFromAcousticEvent(event, effectiveTripodWorldBearingDeg(now))
        val isWorld = worldBearing != null
        val rawAzimuth = event.azimuthDeg.toDouble()
        // Value shown on the card: world bearing when calibrated, else the
        // raw relative angle.
        val displayBearing = worldBearing ?: rawAzimuth

        // De-dupe / debounce on the RAW mic azimuth — it's always present
        // (calibrated or not) and two events from the same physical source
        // have a similar raw angle regardless of calibration state.
        val dismissed = lastDismissedBearing
        if (dismissed != null &&
            now - lastDismissedAtMs < dismissDebounceMs &&
            angularDistance(dismissed, rawAzimuth) <= audioAlertDedupeWindowDeg
        ) {
            return
        }

        val interactive = _uiState.value.selectedTargetId.isNullOrEmpty()
        val current = _activeAudioAlert.value
        val isSameSource = current != null &&
            angularDistance(current.rawAzimuthDeg, rawAzimuth) <= audioAlertDedupeWindowDeg

        _activeAudioAlert.value = if (isSameSource && current != null) {
            // Dedupe-then-replace: refresh confidence/amplitude/duration and
            // bump lastUpdated, but keep firstSeenAtMs so the timeout still
            // counts from the ORIGINAL detection. Re-evaluate the bearing too
            // so a calibration done WHILE an alert is up upgrades it to world.
            current.copy(
                bearingDeg = displayBearing,
                rawAzimuthDeg = rawAzimuth,
                isWorldBearing = isWorld,
                confidence = event.confidence,
                peakAmplitude = event.peakAmplitude,
                durationMs = event.durationMs,
                lastUpdatedAtMs = now,
                isInteractive = interactive,
            )
        } else {
            AudioAlert(
                bearingDeg = displayBearing,
                rawAzimuthDeg = rawAzimuth,
                isWorldBearing = isWorld,
                confidence = event.confidence,
                peakAmplitude = event.peakAmplitude,
                durationMs = event.durationMs,
                firstSeenAtMs = now,
                lastUpdatedAtMs = now,
                isInteractive = interactive,
            )
        }
    }

    private fun updateAlertInteractivity(selectedTargetId: String?) {
        val current = _activeAudioAlert.value ?: return
        val newInteractive = selectedTargetId.isNullOrEmpty()
        if (current.isInteractive != newInteractive) {
            _activeAudioAlert.value = current.copy(isInteractive = newInteractive)
        }
    }

    private fun sweepAlertTimeout() {
        val current = _activeAudioAlert.value ?: return
        val now = System.currentTimeMillis()
        // Accepted alerts get a brief "SLEWING…" notice then clear,
        // independent of the main 20 s auto-dismiss timer.
        if (current.isAccepted && now - current.acceptedAtMs > slewNoticeMs) {
            _activeAudioAlert.value = null
            return
        }
        if (now - current.firstSeenAtMs > audioAlertTimeoutMs) {
            _activeAudioAlert.value = null
        }
    }

    /**
     * Operator tapped SLEW. Marks the alert accepted (so the UI flips
     * to the "SLEWING TO X°…" notice), sends the slew command to the
     * Pi via the repository (the actual contract — `slew_to_bearing`
     * vs `set_servo_angles` — is picked by [SniperRepository.SLEW_COMMAND_MODE]),
     * and lets the periodic sweep clear the alert after [slewNoticeMs].
     *
     * No dismiss-debounce on accept — if events keep arriving from the
     * accepted direction while the Pi auto-scans there, that's expected
     * (the operator chose to engage that direction) and the next event
     * will alert normally.
     *
     * Returns the bearing so callers can log / display it; the side
     * effect (HTTP command + alert flip) is what matters.
     */
    fun acceptAudioAlert(): Double? {
        val current = _activeAudioAlert.value ?: return null
        // Send BOTH so SniperRepository can pick the right one per
        // SLEW_COMMAND_MODE without recomputing anything.
        repository.slewToAcousticContact(
            rawMicAzimuthDeg = current.rawAzimuthDeg,
            worldBearingDeg = current.bearingDeg,
            // Hold the camera's current tilt: the mic array resolves azimuth
            // only, so a slew must not move the elevation the operator set.
            currentServoVerticalDeg = latchedSensorData.value.servoVerticalDeg()?.toDouble(),
        )
        _activeAudioAlert.value = current.copy(
            isAccepted = true,
            acceptedAtMs = System.currentTimeMillis(),
        )
        return current.bearingDeg
    }

    /** Operator dismissed the alert. Clears it and remembers the source's
     *  RAW azimuth so same-source events within dismissDebounceMs don't
     *  immediately re-alert (raw azimuth is the dedupe key — see
     *  applyAcousticEvent). */
    fun dismissAudioAlert() {
        val current = _activeAudioAlert.value ?: return
        lastDismissedBearing = current.rawAzimuthDeg
        lastDismissedAtMs = System.currentTimeMillis()
        _activeAudioAlert.value = null
    }

    /** Shortest-path angular distance between two bearings, in degrees. */
    private fun angularDistance(a: Double, b: Double): Double {
        val d = Math.abs(a - b) % 360.0
        return if (d > 180.0) 360.0 - d else d
    }

    private companion object {
        const val SENSOR_HISTORY_SIZE = 10
    }

    fun navigateToDeviceList() {
        _uiState.value = _uiState.value.copy(currentScreen = AppScreen.DEVICE_SELECTION)
    }

    fun navigateToMap() {
        _uiState.value = _uiState.value.copy(currentScreen = AppScreen.MAP)
    }

    fun navigateToHome() {
        _uiState.value = _uiState.value.copy(currentScreen = AppScreen.HOME)
    }

    fun navigateToDiagnostics() {
        // Remember where we came from so the back button restores it. The
        // dashboard menu state (if open) is in uiState already, so it'll
        // naturally re-render when we navigate back to the dashboard.
        _uiState.value = _uiState.value.copy(
            currentScreen = AppScreen.DIAGNOSTICS,
            diagnosticsFromScreen = _uiState.value.currentScreen,
        )
    }

    fun goBackFromDiagnostics() {
        val target = _uiState.value.diagnosticsFromScreen
        _uiState.value = _uiState.value.copy(
            currentScreen = target,
            diagnosticsFromScreen = AppScreen.HOME,
        )
    }

    /** Open / close the dashboard's MENU dialog. Lives in uiState so its
     *  state survives a round-trip into the Diagnostics screen — the menu
     *  reopens automatically when the operator returns to the dashboard. */
    fun setDashboardMenuOpen(open: Boolean) {
        _uiState.value = _uiState.value.copy(dashboardMenuOpen = open)
    }

    fun selectDevice(device: Device) {
        Log.d("SniperViewModel", "selectDevice called for: ${device.name}")
        _selectedDevice.value = device
        val currentScreen = _uiState.value.currentScreen

        _uiState.value = _uiState.value.copy(
            currentScreen = AppScreen.DASHBOARD,
            selectedDeviceId = device.id,
            previousScreen = currentScreen
        )
        connectToDevice(device)
    }

    private fun connectToDevice(device: Device) {
        Log.d("SniperViewModel", "connectToDevice called for: ${device.name}")
        viewModelScope.launch {
            try {
                val ctx = getApplication<Application>().applicationContext
                // 1. Force traffic over WiFi (RPi AP has no internet → Android may otherwise prefer cellular)
                WifiBinder.bindToWifi(ctx)
                // 2. Auto-detect gateway IP; fall back to the device's stored IP
                val detected = WifiBinder.getGatewayIp(ctx)
                val targetIp = if (detected != WifiBinder.FALLBACK_GATEWAY) detected else device.ipAddress
                Log.d("SniperViewModel", "Connecting to RPi at $targetIp (detected=$detected)")
                // 3. Open WS/HTTP via existing path
                repository.connectToSystem(targetIp)
                Log.d("SniperViewModel", "Connection initiated successfully")
            } catch (e: Exception) {
                Log.e("SniperViewModel", "Connection failed: ${e.message}")
                _uiState.value = _uiState.value.copy(
                    connectionError = "Connection failed: ${e.message}"
                )
            }
        }
    }

    fun goBackFromDashboard() {
        //Smart back: go to where we came from
        val targetScreen = when (_uiState.value.previousScreen) {
            AppScreen.MAP -> AppScreen.MAP
            AppScreen.DEVICE_SELECTION -> AppScreen.DEVICE_SELECTION
            else -> AppScreen.DEVICE_SELECTION  // Default fallback
        }

        Log.d("SniperViewModel", "Going back to: $targetScreen")
        _uiState.value = _uiState.value.copy(
            currentScreen = targetScreen,
            previousScreen = null  // Clear previous screen
        )
    }

    fun goBackToHome() {
        Log.d("SniperViewModel", "goBackToHome called")
        _uiState.value = _uiState.value.copy(currentScreen = AppScreen.HOME)
        _selectedDevice.value = null
        repository.disconnectFromSystem()
        WifiBinder.release(getApplication<Application>().applicationContext)
    }

    fun connectToSystem() {
        _selectedDevice.value?.let { device ->
            connectToDevice(device)
        }
    }

    fun disconnectFromSystem() {
        repository.disconnectFromSystem()
        WifiBinder.release(getApplication<Application>().applicationContext)
    }

    override fun onCleared() {
        super.onCleared()
        // Safety net: release the WiFi binding if the VM dies while still bound.
        WifiBinder.release(getApplication<Application>().applicationContext)
        // Stop GPS updates to spare the battery.
        locationProvider.stop()
    }

    // Command methods

    fun requestCalibration() {
        repository.requestCalibration()
    }

    fun setManualTarget(latitude: Double, longitude: Double) {
        repository.setManualTarget(latitude, longitude)
    }

    fun emergencyStop() {
        repository.emergencyStop()
    }

    fun selectTarget(targetId: String) {
        _uiState.value = _uiState.value.copy(selectedTargetId = targetId)
    }

    fun deselectTarget() {
        _uiState.value = _uiState.value.copy(selectedTargetId = null)
    }

    fun lockTarget(targetId: String) {
        repository.sendLockCommand(targetId, isLocking = true)
    }

    fun unlockTarget(targetId: String) {
        repository.sendLockCommand(targetId, isLocking = false)
    }
}

data class SniperUiState(
    val currentScreen: AppScreen = AppScreen.HOME,
    val isScanning: Boolean = false,
    val selectedDeviceId: String? = null,
    val connectionError: String? = null,
    val isVideoFullscreen: Boolean = false,
    val selectedTargetId: String? = null,
    val previousScreen: AppScreen? = null,
    // Where to return when the operator hits BACK on the Diagnostics
    // screen. Captured by navigateToDiagnostics(), consumed by
    // goBackFromDiagnostics(). Defaults to HOME so the first-launch path
    // (Home → Diagnostics → back) still feels right.
    val diagnosticsFromScreen: AppScreen = AppScreen.HOME,
    // Dashboard MENU dialog visibility. Lifted out of the composable so it
    // persists across a Diagnostics round-trip.
    val dashboardMenuOpen: Boolean = false,
)

enum class AppScreen {
    HOME,
    DEVICE_SELECTION,
    MAP,
    DASHBOARD,
    DIAGNOSTICS
}