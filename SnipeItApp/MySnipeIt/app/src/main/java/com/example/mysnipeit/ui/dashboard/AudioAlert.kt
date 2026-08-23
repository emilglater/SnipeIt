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
import com.example.mysnipeit.data.models.AudioAlert
import com.example.mysnipeit.ui.theme.*
import kotlinx.coroutines.delay

/**
 * Acoustic alert overlay on the dashboard. Two render modes, selected by
 * the [AudioAlert.isInteractive] flag (which the ViewModel flips based on
 * lock state):
 *
 *  - **Card** (interactive) — full card with bearing, confidence,
 *    diagnostic stats, a per-second countdown to auto-dismiss, and two
 *    buttons (SLEW / DISMISS). Shown when no target is locked, so a tap
 *    can never accidentally slew off a live engagement.
 *
 *  - **Chip** (passive) — compact one-liner with bearing + countdown.
 *    No buttons. Shown when a target is locked. The operator can still
 *    see the contact exists; engaging it requires deselecting the target
 *    first, after which the chip upgrades back to the full card
 *    automatically (the ViewModel flips `isInteractive` reactively).
 *
 * Caller passes the timeout constant from [com.example.mysnipeit.viewmodel.SniperViewModel.audioAlertTimeoutMs]
 * so the countdown matches the ViewModel's auto-dismiss policy without
 * the UI carrying its own duplicate timer.
 */
@Composable
fun AudioAlertOverlay(
    alert: AudioAlert,
    timeoutMs: Long,
    onAccept: () -> Unit,
    onDismiss: () -> Unit,
    modifier: Modifier = Modifier,
) {
    // Local 1 Hz ticker so the "Xs remaining" text counts down even when
    // no new events arrive from the Pi. Re-launched when the alert
    // identity changes (a new firstSeenAtMs means a new dedupe group).
    var nowMs by remember(alert.firstSeenAtMs) { mutableStateOf(System.currentTimeMillis()) }
    LaunchedEffect(alert.firstSeenAtMs) {
        while (true) {
            nowMs = System.currentTimeMillis()
            delay(1000)
        }
    }
    val remainingSec = ((alert.firstSeenAtMs + timeoutMs - nowMs) / 1000)
        .coerceAtLeast(0L)

    when {
        // Accepted alerts override both modes — operator just tapped
        // SLEW and we owe them a beat of visual confirmation before the
        // card vanishes.
        alert.isAccepted -> AudioAlertSlewingCard(alert = alert, modifier = modifier)
        alert.isInteractive -> AudioAlertCard(
            alert = alert,
            remainingSec = remainingSec,
            onAccept = onAccept,
            onDismiss = onDismiss,
            modifier = modifier,
        )
        else -> AudioAlertChip(
            alert = alert,
            remainingSec = remainingSec,
            modifier = modifier,
        )
    }
}

/** Direction text for an alert: true-north world bearing when calibrated
 *  ("215°"), else the relative mic-frame angle ("REL -45°"). */
private fun bearingText(alert: AudioAlert): String =
    if (alert.isWorldBearing) "${alert.bearingDeg.toInt()}°"
    else "REL ${alert.rawAzimuthDeg.toInt()}°"

// ----------------------------------------------------------------------------
// Full interactive card (no target locked → operator can act on the contact)
// ----------------------------------------------------------------------------
@Composable
private fun AudioAlertCard(
    alert: AudioAlert,
    remainingSec: Long,
    onAccept: () -> Unit,
    onDismiss: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Column(
        modifier = modifier
            .width(responsiveDp(tablet = 260.dp, compact = 220.dp))
            .background(t.videoChrome)
            .border(1.dp, t.accent),
    ) {
        // Header
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(t.accent.copy(alpha = 0.12f))
                .padding(horizontal = 14.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "♪  AUDIO CONTACT",
                color = t.accent,
                fontSize = 11.sp,
                letterSpacing = 0.2.em,
                fontFamily = JetBrainsMono,
            )
            Text(
                text = "${remainingSec}s",
                color = t.inkDim,
                fontSize = 10.sp,
                letterSpacing = 0.14.em,
                fontFamily = JetBrainsMono,
            )
        }
        // Body
        Column(modifier = Modifier.padding(16.dp)) {
            Lbl(text = if (alert.isWorldBearing) "BEARING" else "BEARING · RELATIVE")
            Spacer(Modifier.height(4.dp))
            Text(
                text = bearingText(alert),
                color = t.ink,
                fontSize = 28.sp,
                letterSpacing = 0.04.em,
                fontFamily = JetBrainsMono,
            )
            if (!alert.isWorldBearing) {
                Spacer(Modifier.height(2.dp))
                Text(
                    text = "uncalibrated — MENU ▸ Calibrate Bearing",
                    color = t.inkMute,
                    fontSize = 9.sp,
                    letterSpacing = 0.1.em,
                    fontFamily = JetBrainsMono,
                )
            }
            Spacer(Modifier.height(12.dp))
            DiagRow(label = "Confidence", value = "${(alert.confidence * 100).toInt()}%")
            DiagRow(label = "Amplitude", value = String.format("%.2f", alert.peakAmplitude))
            DiagRow(label = "Duration", value = "${String.format("%.1f", alert.durationMs)} ms")
        }
        // Buttons
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp)
                .padding(bottom = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            AlertButton(
                label = "DISMISS",
                primary = false,
                onClick = onDismiss,
                modifier = Modifier.weight(1f),
            )
            AlertButton(
                label = "SLEW",
                primary = true,
                onClick = onAccept,
                modifier = Modifier.weight(1f),
            )
        }
    }
}

@Composable
private fun DiagRow(label: String, value: String) {
    val t = LocalTactical.current
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            text = label,
            color = t.inkDim,
            fontSize = 10.sp,
            letterSpacing = 0.14.em,
            fontFamily = JetBrainsMono,
        )
        Text(
            text = value,
            color = t.ink,
            fontSize = 11.sp,
            letterSpacing = 0.06.em,
            fontFamily = JetBrainsMono,
        )
    }
}

@Composable
private fun AlertButton(
    label: String,
    primary: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Box(
        modifier = modifier
            .background(if (primary) t.accent.copy(alpha = 0.18f) else Color.Transparent)
            .border(1.dp, if (primary) t.accent else t.line)
            .clickable(onClick = onClick)
            .padding(vertical = 10.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = label,
            color = if (primary) t.accent else t.ink,
            fontSize = 11.sp,
            letterSpacing = 0.18.em,
            fontFamily = JetBrainsMono,
        )
    }
}

// ----------------------------------------------------------------------------
// Slewing notice (operator just tapped SLEW → brief confirmation card,
// shown for ~1.5 s before the alert clears. Independent of isInteractive
// so the operator gets the same confirmation whether they were in
// card-mode or chip-mode at the moment of tap.)
// ----------------------------------------------------------------------------
@Composable
private fun AudioAlertSlewingCard(
    alert: AudioAlert,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Column(
        modifier = modifier
            .width(responsiveDp(tablet = 260.dp, compact = 220.dp))
            .background(t.videoChrome)
            .border(1.dp, t.accent),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(t.accent.copy(alpha = 0.18f))
                .padding(horizontal = 14.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "♪  SLEWING",
                color = t.accent,
                fontSize = 11.sp,
                letterSpacing = 0.2.em,
                fontFamily = JetBrainsMono,
            )
        }
        Column(modifier = Modifier.padding(16.dp)) {
            Lbl(text = if (alert.isWorldBearing) "BEARING" else "BEARING · RELATIVE")
            Spacer(Modifier.height(4.dp))
            Text(
                text = bearingText(alert),
                color = t.ink,
                fontSize = 28.sp,
                letterSpacing = 0.04.em,
                fontFamily = JetBrainsMono,
            )
            Spacer(Modifier.height(10.dp))
            Text(
                text = "AUTONOMOUS SCAN ENGAGED",
                color = t.inkDim,
                fontSize = 10.sp,
                letterSpacing = 0.14.em,
                fontFamily = JetBrainsMono,
            )
        }
    }
}

// ----------------------------------------------------------------------------
// Passive chip (target locked → no buttons, just awareness)
// ----------------------------------------------------------------------------
@Composable
private fun AudioAlertChip(
    alert: AudioAlert,
    remainingSec: Long,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Row(
        modifier = modifier
            .background(t.videoChrome)
            .border(1.dp, t.accent)
            .padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(
            text = "♪",
            color = t.accent,
            fontSize = 14.sp,
            fontFamily = JetBrainsMono,
        )
        Text(
            text = if (alert.isWorldBearing) "CONTACT" else "CONTACT REL",
            color = t.accent,
            fontSize = 10.sp,
            letterSpacing = 0.2.em,
            fontFamily = JetBrainsMono,
        )
        Text(
            text = bearingText(alert),
            color = t.ink,
            fontSize = 13.sp,
            letterSpacing = 0.06.em,
            fontFamily = JetBrainsMono,
        )
        Text(
            text = "${remainingSec}s",
            color = t.inkDim,
            fontSize = 10.sp,
            letterSpacing = 0.14.em,
            fontFamily = JetBrainsMono,
        )
    }
}
