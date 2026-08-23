package com.example.mysnipeit.ui.dashboard

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.example.mysnipeit.data.ballistics.FiringSolution
import com.example.mysnipeit.data.models.AudioAlert
import com.example.mysnipeit.data.models.ConnectionState
import com.example.mysnipeit.data.models.DetectedTarget
import com.example.mysnipeit.data.models.SensorData
import com.example.mysnipeit.data.models.SystemStatus
import com.example.mysnipeit.data.models.distanceM
import com.example.mysnipeit.data.models.gpsLatLon
import com.example.mysnipeit.data.models.gpsSatellites
import com.example.mysnipeit.data.models.humidityPct
import com.example.mysnipeit.data.models.temperatureC
import com.example.mysnipeit.data.models.windDirectionDeg
import com.example.mysnipeit.data.models.windSpeedMps
import com.example.mysnipeit.ui.theme.*
import kotlinx.coroutines.delay

// Height of the bottom sensor strip. Shared with TacticalVideoPlayer (as its
// bottomOverlayObstruction) because the strip is drawn OVER the bottom of the
// video region — bbox info cards must keep clear of it or their LOCK button
// ends up underneath the strip, visible but not pressable.
private val SensorStripHeight = 68.dp

/**
 * Operator HUD — redesigned to the design's "A layout + dense sensor bar"
 * variant. ALL of the data flow stays — only the chrome around the
 * [TacticalVideoPlayer] changes.
 *
 *  - 36dp [TopBar]: "NODE-CHARLIE · LIVE" + RTSP/CONN chip + back/menu/toggle
 *  - Video fills the rest of the screen with overlays anchored to its edges:
 *      top-left  : TARGETS rail (panel)
 *      top-right : FIRING SOLUTION card (panel)
 *      bottom    : 8-cell sensor strip + UNLOCK button
 *
 * Target reticles inside the video region (corner brackets, bone for
 * tracked, copper for locked) are drawn by [TacticalVideoPlayer]; this
 * file just wraps it in the new chrome.
 */
@Composable
fun DashboardScreen(
    sensorData: SensorData?,
    detectedTargets: List<DetectedTarget>,
    firingSolution: FiringSolution?,
    systemStatus: SystemStatus,
    selectedTargetId: String?,
    streamReady: Boolean,
    rtspStreamUrl: String?,
    audioAlert: AudioAlert?,
    audioAlertTimeoutMs: Long,
    onAudioAlertAccept: () -> Unit,
    onAudioAlertDismiss: () -> Unit,
    tripodCalibratedAtMs: Long?,
    tripodCalibrationTimeoutMs: Long,
    rtspForceTcp: Boolean = true,
    onConnectClick: () -> Unit,
    onDisconnectClick: () -> Unit,
    onTargetSelect: (String) -> Unit = {},
    onTargetLockToggle: (String, Boolean) -> Unit = { _, _ -> },
    onBackClick: () -> Unit = {},
    onMenuClick: () -> Unit = {},
    isDarkTheme: Boolean = true,
    onToggleTheme: () -> Unit = {},
) {
    val t = LocalTactical.current
    val lockedTarget = detectedTargets.firstOrNull { it.id == selectedTargetId }

    // Real video health, reported by TacticalVideoPlayer from actual frame flow
    // (true while frames render, false when stalled/reconnecting). Drives the
    // VIDEO chip — distinct from the WS control-channel state below.
    var videoHealthy by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(t.base),
    ) {
        TopBar(
            device = "OPERATOR · LIVE",
            onBackClick = onBackClick,
        ) {
            // Control-channel (WebSocket) chip — sensors + commands. NOTE this
            // is the WS link, NOT the video; video health is the VIDEO chip.
            val (chipText, chipTone) = when (systemStatus.connectionStatus) {
                ConnectionState.CONNECTED -> "LINK OK" to ChipTone.On
                ConnectionState.CONNECTING -> "LINK…" to ChipTone.Warn
                ConnectionState.DISCONNECTED -> "LINK OFF" to ChipTone.Danger
                ConnectionState.ERROR -> "LINK ERR" to ChipTone.Danger
            }
            Chip(text = chipText, tone = chipTone)

            // Video chip — driven by ACTUAL RTP frame flow, not the WS state.
            // Shown only when a stream is expected. Green when frames are
            // rendering; amber "RECONNECTING" when the watchdog is rebuilding a
            // stalled session (self-healing) so the operator knows video will
            // return on its own.
            if (streamReady) {
                if (videoHealthy) {
                    Chip(text = "VIDEO OK", tone = ChipTone.On)
                } else {
                    Chip(text = "VIDEO…", tone = ChipTone.Warn)
                }
            }

            systemStatus.batteryLevel?.let { bat ->
                Text(
                    text = "BAT $bat%",
                    color = if (bat > 20) t.ink else t.danger,
                    fontSize = 10.sp,
                    letterSpacing = 0.14.em,
                    fontFamily = JetBrainsMono,
                )
            }

            // World-bearing calibration age. Shown only when calibration
            // exists; tone gets warmer (Warn / Danger) as it ages so the
            // operator gets a visual nudge to recalibrate after long
            // sessions. The calibration itself NEVER expires — this is
            // purely a hint.
            tripodCalibratedAtMs?.let { CalibrationAgeChip(it, tripodCalibrationTimeoutMs) }

            // Menu (kept; opens the Diagnostics shortcut dialog from MainActivity)
            TopBarIconButton(label = "MENU", onClick = onMenuClick)
            ThemeToggle(isDark = isDarkTheme, onToggle = onToggleTheme)
        }

        // Video region with overlays
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
        ) {
            TacticalVideoPlayer(
                detectedTargets = detectedTargets,
                firingSolution = firingSolution,
                selectedTargetId = selectedTargetId,
                connectionState = systemStatus.connectionStatus,
                streamReady = streamReady,
                videoStreamUrl = rtspStreamUrl,
                onTargetClick = {},
                onTargetSelect = onTargetSelect,
                onTargetLockToggle = onTargetLockToggle,
                onVideoHealthChanged = { videoHealthy = it },
                forceTcp = rtspForceTcp,
                // The sensor strip overlays the BOTTOM of the video region.
                // The TopBar does NOT overlay it (it stacks above in the
                // Column), so the top obstruction is 0 — cards are still
                // clamped inside the video box so they can never poke up
                // underneath/over the top bar.
                topOverlayObstruction = 0.dp,
                bottomOverlayObstruction = SensorStripHeight,
                modifier = Modifier.fillMaxSize(),
            )

            // Top-left: target list rail
            TargetListRail(
                targets = detectedTargets,
                selectedTargetId = selectedTargetId,
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(16.dp),
            )

            // Top-right: firing-solution card (shown only when a target is
            // locked AND the app's calculator has produced a valid solution).
            if (lockedTarget != null && firingSolution != null) {
                FiringSolutionCard(
                    targetId = lockedTarget.id,
                    solution = firingSolution,
                    modifier = Modifier
                        .align(Alignment.TopEnd)
                        .padding(16.dp)
                        .width(responsiveDp(tablet = 280.dp, compact = 220.dp)),
                )
            }

            // Bottom-right (above the sensor strip): acoustic alert overlay.
            // Renders as a full card (with SLEW / DISMISS) when no target is
            // locked, or as a passive chip when one is — the AudioAlert
            // composable picks the mode from the alert's isInteractive flag,
            // which the ViewModel flips reactively on lock-state changes.
            // bottom padding clears the 68 dp sensor strip + a small gap.
            audioAlert?.let { a ->
                AudioAlertOverlay(
                    alert = a,
                    timeoutMs = audioAlertTimeoutMs,
                    onAccept = onAudioAlertAccept,
                    onDismiss = onAudioAlertDismiss,
                    modifier = Modifier
                        .align(Alignment.BottomEnd)
                        .padding(end = 16.dp, bottom = 84.dp),
                )
            }

            // Bottom: dense 8-cell sensor strip + UNLOCK button.
            // Gate on selectedTargetId (the lock state), NOT on the locked
            // target's detection being present — otherwise the button dies the
            // instant the target leaves the frame, stranding the operator
            // locked. Unlock acts on selectedTargetId so it works even when the
            // detection isn't currently on screen.
            SensorStrip(
                sensorData = sensorData,
                hasLockedTarget = !selectedTargetId.isNullOrEmpty(),
                onUnlock = {
                    selectedTargetId?.let { if (it.isNotEmpty()) onTargetLockToggle(it, false) }
                    onTargetSelect("")
                },
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .fillMaxWidth(),
            )
        }
    }
}

// ----------------------------------------------------------------------------
// World-bearing calibration age chip
//
// Tone bands (timeoutMs = SniperViewModel.tripodCalibrationTimeoutMs):
//   < 30 min        On     (fresh)
//   30 min-timeout  Warn   (consider recalibrating)
//   >= timeout      Danger + "CAL EXP" — the calibration has EXPIRED;
//                   acoustic alerts now show the relative mic angle until
//                   the operator recalibrates.
//
// The amber band is a POC default; the red/expiry point is the actual
// functional timeout passed in from the ViewModel.
// ----------------------------------------------------------------------------
private const val CALIB_AMBER_THRESHOLD_MS = 30L * 60_000L

@Composable
private fun CalibrationAgeChip(calibratedAtMs: Long, timeoutMs: Long) {
    var nowMs by remember(calibratedAtMs) { mutableStateOf(System.currentTimeMillis()) }
    LaunchedEffect(calibratedAtMs) {
        while (true) {
            nowMs = System.currentTimeMillis()
            delay(30_000)
        }
    }
    val ageMs = (nowMs - calibratedAtMs).coerceAtLeast(0L)
    val expired = ageMs >= timeoutMs
    val tone = when {
        expired -> ChipTone.Danger
        ageMs >= CALIB_AMBER_THRESHOLD_MS -> ChipTone.Warn
        else -> ChipTone.On
    }
    Chip(
        text = if (expired) "CAL EXP" else "CAL ${formatCalibrationAgeCompact(ageMs)}",
        tone = tone,
    )
}

// ----------------------------------------------------------------------------
// Top bar small icon button (text-only, mono)
// ----------------------------------------------------------------------------
@Composable
private fun TopBarIconButton(label: String, onClick: () -> Unit) {
    val t = LocalTactical.current
    Box(
        modifier = Modifier
            .border(1.dp, t.line)
            .clickable(onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 4.dp),
    ) {
        Text(
            text = label,
            color = t.ink,
            fontSize = 9.sp,
            letterSpacing = 0.18.em,
            fontFamily = JetBrainsMono,
        )
    }
}

// ----------------------------------------------------------------------------
// Target list rail (top-left)
// ----------------------------------------------------------------------------
@Composable
private fun TargetListRail(
    targets: List<DetectedTarget>,
    selectedTargetId: String?,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Column(
        modifier = modifier
            .widthIn(min = 200.dp)
            .background(t.videoChrome)
            .border(1.dp, t.lineHi)
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Lbl(text = "TARGETS · ${targets.size}")
        Spacer(Modifier.height(4.dp))
        if (targets.isEmpty()) {
            Text(
                text = "NO CONTACTS",
                color = t.inkMute,
                fontSize = 10.sp,
                letterSpacing = 0.14.em,
                fontFamily = JetBrainsMono,
            )
        } else {
            targets.take(6).forEach { target ->
                TargetRailRow(target = target, isLocked = target.id == selectedTargetId)
            }
            if (targets.size > 6) {
                Text(
                    text = "+ ${targets.size - 6} MORE",
                    color = t.inkMute,
                    fontSize = 9.sp,
                    letterSpacing = 0.18.em,
                    fontFamily = JetBrainsMono,
                )
            }
        }
    }
}

@Composable
private fun TargetRailRow(target: DetectedTarget, isLocked: Boolean) {
    val t = LocalTactical.current
    Row(
        modifier = Modifier
            .drawBehind {
                if (isLocked) {
                    drawLine(
                        color = t.accent,
                        start = Offset(0f, 0f),
                        end = Offset(0f, size.height),
                        strokeWidth = 2.dp.toPx(),
                    )
                }
            }
            .padding(start = if (isLocked) 0.dp else 8.dp, top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = target.id,
            color = if (isLocked) t.accent else t.ink,
            fontSize = 13.sp,
            letterSpacing = 0.1.em,
            fontFamily = JetBrainsMono,
            modifier = Modifier.width(46.dp),
        )
        Text(
            text = target.targetType.uppercase(),
            color = t.inkDim,
            fontSize = 10.sp,
            letterSpacing = 0.12.em,
            fontFamily = JetBrainsMono,
            modifier = Modifier.width(80.dp),
        )
        Text(
            text = "${(target.confidence * 100).toInt()}%",
            color = t.ink,
            fontSize = 11.sp,
            fontFamily = JetBrainsMono,
        )
    }
}

// ----------------------------------------------------------------------------
// Firing solution card (top-right) — shown when a target is locked AND
// the app's ballistic calculator has produced a valid solution. All angles
// are degrees (no MIL / MOA); the elevation field is the HOLD-OVER above
// the target the operator should apply, not the absolute muzzle elevation.
// ----------------------------------------------------------------------------
@Composable
private fun FiringSolutionCard(
    targetId: String,
    solution: FiringSolution,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Column(
        modifier = modifier
            .background(t.videoChrome)
            .border(1.dp, t.lineHi),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .drawBehind {
                    drawLine(
                        color = t.line,
                        start = Offset(0f, size.height),
                        end = Offset(size.width, size.height),
                        strokeWidth = 1.dp.toPx(),
                    )
                }
                .padding(horizontal = 14.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Lbl(
                text = "FIRING SOLUTION · $targetId",
                modifier = Modifier.weight(1f, fill = false),
            )
            Spacer(Modifier.width(8.dp))
            Chip(text = "LOCKED", tone = ChipTone.Warn)
        }
        Column(modifier = Modifier.padding(16.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Stat(
                    label = "Range",
                    value = "${solution.rangeM.toInt()}",
                    unit = "m",
                    modifier = Modifier.weight(1f),
                )
                Stat(
                    label = "Azimuth",
                    value = "${solution.azimuthDeg.toInt()}°",
                    modifier = Modifier.weight(1f),
                )
            }
            Spacer(Modifier.height(12.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Stat(
                    label = "Hold ↑",
                    value = formatAngleDeg(solution.elevationDeg),
                    modifier = Modifier.weight(1f),
                )
                Stat(
                    label = "Hold →",
                    value = formatAngleDeg(solution.windageDeg),
                    modifier = Modifier.weight(1f),
                )
            }
            Spacer(Modifier.height(12.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Stat(
                    label = "TOF",
                    value = String.format("%.2f", solution.timeOfFlightS),
                    unit = "s",
                    modifier = Modifier.weight(1f),
                )
                Stat(
                    label = "Confidence",
                    value = "${(solution.confidence * 100).toInt()}%",
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}

/**
 * Format an angle for the HUD. Sub-degree values get one decimal (so a
 * 100 m shot reads "+0.1°" rather than "+0°"); larger holds round to the
 * integer degree to keep the card tight.
 */
private fun formatAngleDeg(deg: Double): String {
    val sign = if (deg >= 0) "+" else "−"
    val mag = Math.abs(deg)
    return if (mag < 10.0) "$sign${String.format("%.1f", mag)}°"
    else "$sign${mag.toInt()}°"
}

// ----------------------------------------------------------------------------
// Sensor strip (bottom) — 8 cells + UNLOCK
// ----------------------------------------------------------------------------
@Composable
private fun SensorStrip(
    sensorData: SensorData?,
    hasLockedTarget: Boolean,
    onUnlock: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    // FIXED height: without this, cells using fillMaxHeight() inside a
    // BottomStart-aligned Box-child would expand to the Box's full height
    // (the whole video region) — which is exactly what the user reported.
    Row(
        modifier = modifier
            .height(SensorStripHeight)
            .background(t.panel)
            .drawBehind {
                drawLine(
                    color = t.line,
                    start = Offset(0f, 0f),
                    end = Offset(size.width, 0f),
                    strokeWidth = 1.dp.toPx(),
                )
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Real values come from the ddl_frame nested structure via the helper
        // extensions in data/models/SensorData.kt. Each helper returns null
        // when its sub-frame is absent or its `valid` flag is false, which we
        // render as "—".
        SensorCell(
            label = "TEMPERATURE",
            value = sensorData.temperatureC()?.let { "${it.toInt()}°C" } ?: "—",
            modifier = Modifier.weight(1f),
        )
        SensorCell(
            label = "HUMIDITY",
            value = sensorData.humidityPct()?.let { "${it.toInt()}%" } ?: "—",
            modifier = Modifier.weight(1f),
        )
        // Wind speed + direction come from the Pi's WindFrame (two
        // independent valid flags — one channel can read while the other
        // is invalid). On compact screens we drop both cells to give the
        // remaining ones more breathing room.
        if (!isCompactWidth()) {
            SensorCell(
                label = "WIND_SPD",
                value = sensorData.windSpeedMps()?.let { String.format("%.1f m/s", it) } ?: "—",
                modifier = Modifier.weight(1f),
            )
            SensorCell(
                label = "WIND_DIR",
                value = sensorData.windDirectionDeg()?.let { "${it.toInt()}°" } ?: "—",
                modifier = Modifier.weight(1f),
            )
        }
        // GPS — lat, lon, and (when valid) satellite count packed into one cell.
        SensorCell(
            label = "GPS",
            value = sensorData.gpsLatLon()?.let { (lat, lon) ->
                val sats = sensorData.gpsSatellites()
                if (sats != null) String.format("%.3f, %.3f · %d SAT", lat, lon, sats)
                else              String.format("%.3f, %.3f", lat, lon)
            } ?: "—",
            modifier = Modifier.weight(1.6f),
        )
        SensorCell(
            label = "DISTANCE",
            value = sensorData.distanceM()?.let { "${it.toInt()}m" } ?: "—",
            modifier = Modifier.weight(1f),
            hideRightBorder = true,
        )
        // UNLOCK action (only enabled if there's something to unlock)
        Box(
            modifier = Modifier
                .background(t.panelHi)
                .drawBehind {
                    drawLine(
                        color = t.lineHi,
                        start = Offset(0f, 0f),
                        end = Offset(0f, size.height),
                        strokeWidth = 1.dp.toPx(),
                    )
                }
                .clickable(enabled = hasLockedTarget, onClick = onUnlock)
                .padding(horizontal = 28.dp, vertical = 14.dp),
        ) {
            Text(
                text = "UNLOCK",
                color = if (hasLockedTarget) t.ink else t.inkMute,
                fontSize = 11.sp,
                letterSpacing = 0.2.em,
                fontFamily = JetBrainsMono,
            )
        }
    }
}

@Composable
private fun SensorCell(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    hideRightBorder: Boolean = false,
) {
    val t = LocalTactical.current
    Column(
        modifier = modifier
            .fillMaxHeight()  // OK now — parent Row has a fixed 68dp height
            .drawBehind {
                if (!hideRightBorder) {
                    drawLine(
                        color = t.line,
                        start = Offset(size.width, 0f),
                        end = Offset(size.width, size.height),
                        strokeWidth = 1.dp.toPx(),
                    )
                }
            }
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.Center,
    ) {
        Lbl(text = label)
        Spacer(Modifier.height(4.dp))
        Text(
            text = value,
            color = t.ink,
            fontSize = 13.sp,
            letterSpacing = 0.06.em,
            fontFamily = JetBrainsMono,
        )
    }
}
