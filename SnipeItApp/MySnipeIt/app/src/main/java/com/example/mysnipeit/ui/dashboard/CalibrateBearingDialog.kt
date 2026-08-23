package com.example.mysnipeit.ui.dashboard

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.example.mysnipeit.ui.theme.*
import kotlinx.coroutines.delay

/**
 * Calibrate the tripod-forward world bearing. Operator centres the
 * camera at servo 90° (pointing at the tripod's forward direction),
 * confirms the compass has fixed, then taps CAPTURE — the current
 * compass reading is saved as the offset that converts mic-frame
 * azimuths into world-frame bearings.
 *
 * The dialog shows the live compass reading and the last calibrated
 * value (if any) so the operator can verify before saving. The
 * CAPTURE callback returns the saved value (or null if no compass
 * fix) and the dialog updates to reflect it without closing — the
 * operator dismisses explicitly.
 */
@Composable
fun CalibrateBearingDialog(
    liveCompassDeg: Float?,
    currentCalibrationDeg: Double?,
    currentCalibratedAtMs: Long?,
    calibrationTimeoutMs: Long,
    onCapture: () -> Double?,
    onDismiss: () -> Unit,
) {
    val t = LocalTactical.current
    // Local state so the dialog shows the NEW value immediately after a
    // successful capture, without waiting for the StateFlow round-trip.
    var lastCaptured by remember { mutableStateOf<Double?>(null) }
    var lastCapturedAtMs by remember { mutableStateOf<Long?>(null) }
    val displayCalibration = lastCaptured ?: currentCalibrationDeg
    val displayCalibratedAtMs = lastCapturedAtMs ?: currentCalibratedAtMs

    // 30 s ticker so the "X min ago" line counts up while the dialog is
    // open. Keyed to the calibration timestamp so a fresh capture
    // restarts the clock immediately.
    var nowMs by remember(displayCalibratedAtMs) {
        mutableStateOf(System.currentTimeMillis())
    }
    LaunchedEffect(displayCalibratedAtMs) {
        while (true) {
            nowMs = System.currentTimeMillis()
            delay(30_000)
        }
    }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Column(
            modifier = Modifier
                .width(responsiveDp(tablet = 480.dp, compact = 380.dp))
                .background(t.panel)
                .border(1.dp, t.lineHi)
                .padding(20.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Lbl(text = "CALIBRATE WORLD BEARING")
                CloseBtn(onDismiss)
            }
            Spacer(Modifier.height(14.dp))

            Text(
                text = "Center the camera at servo 90° (tripod-forward) and wait for the compass to settle. Tap CAPTURE to save the current heading as the tripod-forward world bearing — every acoustic alert's world direction is computed from this offset until you re-calibrate.",
                color = t.ink,
                fontSize = 11.sp,
                letterSpacing = 0.06.em,
                fontFamily = JetBrainsMono,
            )
            Spacer(Modifier.height(16.dp))

            // Two stat cards side by side: live compass + saved value.
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                StatBox(
                    label = "LIVE COMPASS",
                    value = liveCompassDeg?.let { "${it.toInt()}°" } ?: "—",
                    accent = liveCompassDeg != null,
                    modifier = Modifier.weight(1f),
                )
                StatBox(
                    label = "SAVED OFFSET",
                    value = displayCalibration?.let { "${it.toInt()}°" } ?: "—",
                    accent = displayCalibration != null,
                    modifier = Modifier.weight(1f),
                )
            }
            if (displayCalibratedAtMs != null) {
                Spacer(Modifier.height(6.dp))
                val ageMs = nowMs - displayCalibratedAtMs
                val expired = ageMs >= calibrationTimeoutMs
                Text(
                    text = "Last calibrated: ${formatCalibrationAge(ageMs)}" +
                        if (expired) " · EXPIRED — alerts show relative angle" else "",
                    color = if (expired) t.danger else t.inkDim,
                    fontSize = 10.sp,
                    letterSpacing = 0.14.em,
                    fontFamily = JetBrainsMono,
                )
            }
            Spacer(Modifier.height(16.dp))

            if (liveCompassDeg == null) {
                Text(
                    text = "Compass not fixed yet. Wait for a heading reading before calibrating.",
                    color = t.danger,
                    fontSize = 10.sp,
                    letterSpacing = 0.14.em,
                    fontFamily = JetBrainsMono,
                )
                Spacer(Modifier.height(12.dp))
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                CalBtn(
                    label = "CAPTURE",
                    primary = true,
                    enabled = liveCompassDeg != null,
                    onClick = {
                        val captured = onCapture()
                        if (captured != null) {
                            lastCaptured = captured
                            lastCapturedAtMs = System.currentTimeMillis()
                        }
                    },
                    modifier = Modifier.weight(1f),
                )
                CalBtn(
                    label = "CLOSE",
                    primary = false,
                    enabled = true,
                    onClick = onDismiss,
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}

@Composable
private fun StatBox(
    label: String,
    value: String,
    accent: Boolean,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Column(
        modifier = modifier
            .background(t.panelHi)
            .border(1.dp, if (accent) t.accent else t.line)
            .padding(12.dp),
    ) {
        Lbl(text = label)
        Spacer(Modifier.height(4.dp))
        Text(
            text = value,
            color = if (accent) t.accent else t.inkMute,
            fontSize = 22.sp,
            letterSpacing = 0.04.em,
            fontFamily = JetBrainsMono,
        )
    }
}

@Composable
private fun CalBtn(
    label: String,
    primary: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Box(
        modifier = modifier
            .background(
                if (primary && enabled) t.accent.copy(alpha = 0.18f)
                else Color.Transparent
            )
            .border(1.dp, when {
                !enabled -> t.line
                primary -> t.accent
                else -> t.lineHi
            })
            .clickable(enabled = enabled, onClick = onClick)
            .padding(vertical = 12.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = label,
            color = when {
                !enabled -> t.inkMute
                primary -> t.accent
                else -> t.ink
            },
            fontSize = 11.sp,
            letterSpacing = 0.18.em,
            fontFamily = JetBrainsMono,
        )
    }
}

/**
 * Human-readable calibration age for the dialog's "Last calibrated: X"
 * line. Minute-grain (the underlying ticker updates every 30 s, so
 * second-level precision would be misleading).
 */
internal fun formatCalibrationAge(ageMs: Long): String {
    val min = ageMs.coerceAtLeast(0L) / 60_000L
    val hr = min / 60L
    val days = hr / 24L
    return when {
        min < 1 -> "just now"
        min < 60 -> "$min min ago"
        hr < 24 -> "$hr h ${min % 60} min ago"
        else -> "$days d ago"
    }
}

/**
 * Compact form for the dashboard top-bar chip ("CAL 12m"). Same
 * minute-grain as [formatCalibrationAge].
 */
internal fun formatCalibrationAgeCompact(ageMs: Long): String {
    val min = ageMs.coerceAtLeast(0L) / 60_000L
    val hr = min / 60L
    return when {
        min < 1 -> "<1m"
        min < 60 -> "${min}m"
        hr < 24 -> "${hr}h${(min % 60).toString().padStart(2, '0')}"
        else -> "${hr / 24}d"
    }
}

@Composable
private fun CloseBtn(onClick: () -> Unit) {
    val t = LocalTactical.current
    Box(
        modifier = Modifier
            .border(1.dp, t.line)
            .clickable(onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 4.dp),
    ) {
        Text(
            text = "✕",
            color = t.ink,
            fontSize = 11.sp,
            fontFamily = JetBrainsMono,
        )
    }
}
