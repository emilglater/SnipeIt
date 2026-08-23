package com.example.mysnipeit.ui.device

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
import androidx.compose.ui.window.Dialog
import com.example.mysnipeit.data.models.Device
import com.example.mysnipeit.data.models.DeviceStatus
import com.example.mysnipeit.ui.theme.*

/**
 * Device list screen — design B's card vocabulary rendered as
 * vertically-stacked rows (the user's final pick).
 *
 * Layout:
 *  - 36dp [TopBar] with "UNIT REGISTRY · N" device count
 *  - Left 160dp sidebar nav: DEVICES (active) / MAP / LOGS / DIAG
 *  - Right pane:
 *      - Header: UNITS · LIVE label + "SELECT TO CONNECT" + filter tabs
 *      - Stacked rows — each row is a 7-cell horizontal table:
 *            letter / name+sector+IP / status chip / battery bar / link /
 *            range / action (CONNECT / DETAILS / UNREACHABLE)
 */
@Composable
fun DeviceSelectionScreen(
    devices: List<Device>,
    onDeviceSelected: (Device) -> Unit,
    onBackClick: () -> Unit,
    onMapClick: () -> Unit = {},
    onDiagnosticsClick: () -> Unit = {},
    isDarkTheme: Boolean = true,
    onToggleTheme: () -> Unit = {},
) {
    val t = LocalTactical.current
    var selectedDevice by remember { mutableStateOf<Device?>(null) }
    var showInactiveWarning by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(t.base),
    ) {
        TopBar(
            device = "UNIT REGISTRY · ${devices.size}",
            onBackClick = onBackClick,
        ) {
            ThemeToggle(isDark = isDarkTheme, onToggle = onToggleTheme)
        }
        Row(modifier = Modifier.fillMaxSize()) {
            SidebarNav(
                count = devices.size,
                onMapClick = onMapClick,
                onDiagnosticsClick = onDiagnosticsClick,
            )
            DeviceListPane(
                devices = devices,
                onPick = { device ->
                    if (device.status == DeviceStatus.INACTIVE) {
                        showInactiveWarning = true
                    } else {
                        selectedDevice = device
                    }
                },
            )
        }
    }

    selectedDevice?.let { device ->
        DeviceDetailsDialog(
            device = device,
            onDismiss = { selectedDevice = null },
            onConnect = {
                onDeviceSelected(device)
                selectedDevice = null
            },
        )
    }
    if (showInactiveWarning) {
        InactiveDeviceDialog(onDismiss = { showInactiveWarning = false })
    }
}

// ----------------------------------------------------------------------------
// Sidebar nav (left rail)
//
// LOGS removed entirely — the app doesn't have a log screen, so showing the
// label was misleading. The other items are wired to real navigation:
//   DEVICES → no-op (we're already here)
//   MAP     → onMapClick
//   DIAG    → onDiagnosticsClick
// ----------------------------------------------------------------------------
@Composable
private fun SidebarNav(
    count: Int,
    onMapClick: () -> Unit,
    onDiagnosticsClick: () -> Unit,
) {
    val t = LocalTactical.current
    Column(
        modifier = Modifier
            .width(responsiveDp(tablet = 160.dp, compact = 110.dp))
            .fillMaxHeight()
            .background(t.panel)
            .drawBehind {
                drawLine(
                    color = t.line,
                    start = Offset(size.width, 0f),
                    end = Offset(size.width, size.height),
                    strokeWidth = 1.dp.toPx(),
                )
            }
            .padding(vertical = 20.dp),
    ) {
        SidebarNavItem(label = "DEVICES", badge = count.toString(), active = true, onClick = null)
        SidebarNavItem(label = "MAP",     badge = "", active = false, onClick = onMapClick)
        SidebarNavItem(label = "DIAG",    badge = "", active = false, onClick = onDiagnosticsClick)
    }
}

@Composable
private fun SidebarNavItem(
    label: String,
    badge: String,
    active: Boolean,
    onClick: (() -> Unit)?,
) {
    val t = LocalTactical.current
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (active) t.panelHi else Color.Transparent)
            .drawBehind {
                if (active) {
                    drawLine(
                        color = t.accent,
                        start = Offset(0f, 0f),
                        end = Offset(0f, size.height),
                        strokeWidth = 2.dp.toPx(),
                    )
                }
            }
            .let { m -> if (onClick != null) m.clickable(onClick = onClick) else m }
            .padding(horizontal = 22.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            text = label,
            color = if (active) t.ink else t.inkDim,
            fontSize = 11.sp,
            letterSpacing = 0.18.em,
            fontFamily = JetBrainsMono,
        )
        if (badge.isNotEmpty()) {
            Text(
                text = badge,
                color = t.inkMute,
                fontSize = 9.sp,
                fontFamily = JetBrainsMono,
            )
        }
    }
}

// ----------------------------------------------------------------------------
// Right pane — header + filter tabs + stacked rows
// ----------------------------------------------------------------------------
@Composable
private fun DeviceListPane(
    devices: List<Device>,
    onPick: (Device) -> Unit,
) {
    val t = LocalTactical.current
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(24.dp),
    ) {
        // Header row: label + filter tabs
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.Bottom,
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Column {
                Lbl(text = "UNITS · LIVE")
                Spacer(Modifier.height(4.dp))
                Text(
                    text = "SELECT TO CONNECT",
                    color = t.ink,
                    fontSize = 22.sp,
                    letterSpacing = 0.06.em,
                    fontFamily = JetBrainsMono,
                )
            }
            // Filter tabs — purely cosmetic for now; counts derived live
            val activeCount = devices.count { it.status == DeviceStatus.ACTIVE }
            val offlineCount = devices.count { it.status == DeviceStatus.INACTIVE || it.status == DeviceStatus.ERROR }
            Row {
                FilterTab(text = "ALL · ${devices.size}",  active = true)
                FilterTab(text = "ACTIVE · $activeCount", active = false)
                FilterTab(text = "OFFLINE · $offlineCount", active = false)
            }
        }
        Spacer(Modifier.height(18.dp))
        // Stacked device rows
        LazyColumn(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            items(devices) { device ->
                DeviceRow(device = device, onClick = { onPick(device) })
            }
        }
    }
}

@Composable
private fun FilterTab(text: String, active: Boolean) {
    val t = LocalTactical.current
    Box(
        modifier = Modifier
            .border(1.dp, t.line)
            .background(if (active) t.panel else Color.Transparent)
            .padding(horizontal = 14.dp, vertical = 8.dp)
    ) {
        Text(
            text = text,
            color = if (active) t.ink else t.inkDim,
            fontSize = 10.sp,
            letterSpacing = 0.18.em,
            fontFamily = JetBrainsMono,
        )
    }
}

// ----------------------------------------------------------------------------
// One device row
// ----------------------------------------------------------------------------
@Composable
private fun DeviceRow(device: Device, onClick: () -> Unit) {
    val isActive = device.status == DeviceStatus.ACTIVE
    val isOffline = device.status == DeviceStatus.INACTIVE || device.status == DeviceStatus.ERROR

    // Letter we display in the left cell — derive from the trailing digit of the id.
    val letter = remember(device.id) {
        when {
            device.id.endsWith("1") -> "A"
            device.id.endsWith("2") -> "B"
            device.id.endsWith("3") -> "C"
            device.id.endsWith("4") -> "D"
            else -> device.id.lastOrNull()?.uppercaseChar()?.toString() ?: "?"
        }
    }

    // On phones the 7-fixed-cell horizontal row (~656dp) overflows the screen
    // so we render a stacked-card layout instead — same data, two rows.
    if (isCompactWidth()) {
        DeviceRowCompact(device, letter, isActive, isOffline, onClick)
    } else {
        DeviceRowTablet(device, letter, isActive, isOffline, onClick)
    }
}

@Composable
private fun DeviceRowTablet(
    device: Device,
    letter: String,
    isActive: Boolean,
    isOffline: Boolean,
    onClick: () -> Unit,
) {
    val t = LocalTactical.current

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (isActive) t.panelHi else t.panel)
            .border(1.dp, if (isActive) t.lineHi else t.line)
            .clickable(enabled = !isOffline, onClick = onClick)
            .height(IntrinsicSize.Min),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Letter cell
        Box(
            modifier = Modifier
                .width(56.dp)
                .fillMaxHeight()
                .drawBehind {
                    drawLine(
                        color = t.line,
                        start = Offset(size.width, 0f),
                        end = Offset(size.width, size.height),
                        strokeWidth = 1.dp.toPx(),
                    )
                },
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = letter,
                color = t.ink,
                fontSize = 18.sp,
                fontFamily = JetBrainsMono,
            )
        }
        // Name + sector + IP
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(horizontal = 18.dp, vertical = 14.dp)
                .drawBehind {
                    drawLine(
                        color = t.line,
                        start = Offset(size.width, 0f),
                        end = Offset(size.width, size.height),
                        strokeWidth = 1.dp.toPx(),
                    )
                },
            verticalArrangement = Arrangement.Center,
        ) {
            Text(
                text = device.name.uppercase(),
                color = t.ink,
                fontSize = 13.sp,
                letterSpacing = 0.1.em,
                fontFamily = JetBrainsMono,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = "${device.location.uppercase()} · ${device.ipAddress}",
                color = t.inkDim,
                fontSize = 9.sp,
                letterSpacing = 0.2.em,
                fontFamily = JetBrainsMono,
            )
        }
        // Status chip
        CellWithRightBorder(width = 110.dp) {
            val tone = when (device.status) {
                DeviceStatus.ACTIVE -> ChipTone.On
                DeviceStatus.INACTIVE -> ChipTone.Danger
                DeviceStatus.SCANNING -> ChipTone.Warn
                DeviceStatus.ERROR -> ChipTone.Danger
            }
            val statusText = when (device.status) {
                DeviceStatus.ACTIVE -> "ACTIVE"
                DeviceStatus.INACTIVE -> "OFFLINE"
                DeviceStatus.SCANNING -> "STANDBY"
                DeviceStatus.ERROR -> "ERROR"
            }
            Chip(text = statusText, tone = tone)
        }
        // Battery bar
        CellWithRightBorder(width = 130.dp) {
            Column {
                Lbl(text = "BATT")
                Spacer(Modifier.height(6.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(
                        modifier = Modifier
                            .width(48.dp)
                            .height(4.dp)
                            .background(t.line)
                    ) {
                        Box(
                            modifier = Modifier
                                .fillMaxHeight()
                                .fillMaxWidth(device.batteryLevel / 100f)
                                .background(t.lineHi)
                        )
                    }
                    Spacer(Modifier.width(8.dp))
                    Text(
                        text = "${device.batteryLevel}%",
                        color = t.ink,
                        fontSize = 13.sp,
                        fontFamily = JetBrainsMono,
                    )
                }
            }
        }
        // Link (we don't have real latency yet; show dash for offline, OK otherwise)
        CellWithRightBorder(width = 110.dp) {
            Column {
                Lbl(text = "LINK")
                Spacer(Modifier.height(4.dp))
                Text(
                    text = if (isOffline) "—" else "OK",
                    color = t.ink,
                    fontSize = 14.sp,
                    fontFamily = JetBrainsMono,
                )
            }
        }
        // Range (placeholder; the app doesn't compute distance to nodes yet)
        CellWithRightBorder(width = 110.dp) {
            Column {
                Lbl(text = "RANGE")
                Spacer(Modifier.height(4.dp))
                Row(verticalAlignment = Alignment.Bottom) {
                    Text(
                        text = "—",
                        color = t.ink,
                        fontSize = 14.sp,
                        fontFamily = JetBrainsMono,
                    )
                    Spacer(Modifier.width(4.dp))
                    Text(
                        text = "KM",
                        color = t.inkMute,
                        fontSize = 10.sp,
                        fontFamily = JetBrainsMono,
                    )
                }
            }
        }
        // Action cell
        Box(
            modifier = Modifier
                .width(140.dp)
                .fillMaxHeight()
                .padding(horizontal = 8.dp),
            contentAlignment = Alignment.Center,
        ) {
            when {
                isOffline -> Text(
                    text = "UNREACHABLE",
                    color = t.danger,
                    fontSize = 10.sp,
                    letterSpacing = 0.18.em,
                    fontFamily = JetBrainsMono,
                )
                else -> Box(
                    modifier = Modifier
                        .background(if (isActive) t.panelHi else Color.Transparent)
                        .border(1.dp, t.lineHi)
                        .clickable(onClick = onClick)
                        .padding(horizontal = 18.dp, vertical = 10.dp)
                ) {
                    Text(
                        text = if (isActive) "CONNECT →" else "DETAILS →",
                        color = t.ink,
                        fontSize = 11.sp,
                        letterSpacing = 0.18.em,
                        fontFamily = JetBrainsMono,
                    )
                }
            }
        }
    }
}

/**
 * Compact / phone layout: two stacked rows instead of seven horizontal cells.
 *  Row 1: letter | name + sector + IP | status chip
 *  Row 2: BATT bar + % | LINK | action button
 * RANGE is dropped here since the app only ever has "—" for it anyway.
 */
@Composable
private fun DeviceRowCompact(
    device: Device,
    letter: String,
    isActive: Boolean,
    isOffline: Boolean,
    onClick: () -> Unit,
) {
    val t = LocalTactical.current

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (isActive) t.panelHi else t.panel)
            .border(1.dp, if (isActive) t.lineHi else t.line)
            .clickable(enabled = !isOffline, onClick = onClick),
    ) {
        // ---- Row 1 — letter | name | status chip ----
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .height(IntrinsicSize.Min),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier
                    .width(50.dp)
                    .fillMaxHeight()
                    .drawBehind {
                        drawLine(
                            color = t.line,
                            start = Offset(size.width, 0f),
                            end = Offset(size.width, size.height),
                            strokeWidth = 1.dp.toPx(),
                        )
                    },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = letter,
                    color = t.ink,
                    fontSize = 18.sp,
                    fontFamily = JetBrainsMono,
                )
            }
            Column(
                modifier = Modifier
                    .weight(1f)
                    .padding(horizontal = 14.dp, vertical = 10.dp),
            ) {
                Text(
                    text = device.name.uppercase(),
                    color = t.ink,
                    fontSize = 13.sp,
                    letterSpacing = 0.1.em,
                    fontFamily = JetBrainsMono,
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    text = "${device.location.uppercase()} · ${device.ipAddress}",
                    color = t.inkDim,
                    fontSize = 9.sp,
                    letterSpacing = 0.18.em,
                    fontFamily = JetBrainsMono,
                )
            }
            val tone = when (device.status) {
                DeviceStatus.ACTIVE -> ChipTone.On
                DeviceStatus.INACTIVE -> ChipTone.Danger
                DeviceStatus.SCANNING -> ChipTone.Warn
                DeviceStatus.ERROR -> ChipTone.Danger
            }
            val statusText = when (device.status) {
                DeviceStatus.ACTIVE -> "ACTIVE"
                DeviceStatus.INACTIVE -> "OFFLINE"
                DeviceStatus.SCANNING -> "STANDBY"
                DeviceStatus.ERROR -> "ERROR"
            }
            Box(modifier = Modifier.padding(end = 12.dp)) {
                Chip(text = statusText, tone = tone)
            }
        }

        // ---- horizontal hairline divider ----
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(t.line)
        )

        // ---- Row 2 — BATT | LINK | action ----
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            // BATT bar
            Row(
                modifier = Modifier.weight(1f),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Lbl(text = "BATT")
                Spacer(Modifier.width(6.dp))
                Box(
                    modifier = Modifier
                        .width(40.dp)
                        .height(4.dp)
                        .background(t.line)
                ) {
                    Box(
                        modifier = Modifier
                            .fillMaxHeight()
                            .fillMaxWidth(device.batteryLevel / 100f)
                            .background(t.lineHi)
                    )
                }
                Spacer(Modifier.width(6.dp))
                Text(
                    text = "${device.batteryLevel}%",
                    color = t.ink,
                    fontSize = 12.sp,
                    fontFamily = JetBrainsMono,
                )
            }
            // LINK
            Row(verticalAlignment = Alignment.CenterVertically) {
                Lbl(text = "LINK")
                Spacer(Modifier.width(6.dp))
                Text(
                    text = if (isOffline) "—" else "OK",
                    color = t.ink,
                    fontSize = 12.sp,
                    fontFamily = JetBrainsMono,
                )
            }
            Spacer(Modifier.width(12.dp))
            // Action
            when {
                isOffline -> Text(
                    text = "UNREACHABLE",
                    color = t.danger,
                    fontSize = 9.sp,
                    letterSpacing = 0.18.em,
                    fontFamily = JetBrainsMono,
                )
                else -> Box(
                    modifier = Modifier
                        .background(if (isActive) t.panelHi else Color.Transparent)
                        .border(1.dp, t.lineHi)
                        .clickable(onClick = onClick)
                        .padding(horizontal = 14.dp, vertical = 6.dp)
                ) {
                    Text(
                        text = if (isActive) "CONNECT →" else "DETAILS →",
                        color = t.ink,
                        fontSize = 10.sp,
                        letterSpacing = 0.18.em,
                        fontFamily = JetBrainsMono,
                    )
                }
            }
        }
    }
}

@Composable
private fun CellWithRightBorder(width: androidx.compose.ui.unit.Dp, content: @Composable () -> Unit) {
    val t = LocalTactical.current
    Box(
        modifier = Modifier
            .width(width)
            .fillMaxHeight()
            .padding(horizontal = 16.dp, vertical = 14.dp)
            .drawBehind {
                drawLine(
                    color = t.line,
                    start = Offset(size.width + 16.dp.toPx(), 0f),
                    end = Offset(size.width + 16.dp.toPx(), size.height),
                    strokeWidth = 1.dp.toPx(),
                )
            },
        contentAlignment = Alignment.CenterStart,
    ) { content() }
}

// ----------------------------------------------------------------------------
// Dialogs — restyled to the tactical palette
// ----------------------------------------------------------------------------
@Composable
private fun InactiveDeviceDialog(onDismiss: () -> Unit) {
    val t = LocalTactical.current
    Dialog(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .background(t.panel)
                .border(1.dp, t.danger)
                .padding(24.dp)
                .widthIn(min = 300.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text = "CONNECTION UNAVAILABLE",
                color = t.danger,
                fontSize = 14.sp,
                letterSpacing = 0.18.em,
                fontFamily = JetBrainsMono,
            )
            Spacer(Modifier.height(16.dp))
            Text(
                text = "Cannot connect to offline node.",
                color = t.ink,
                fontSize = 13.sp,
                fontFamily = JetBrainsMono,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = "Ensure the device is powered on and in range.",
                color = t.inkDim,
                fontSize = 12.sp,
                fontFamily = JetBrainsMono,
            )
            Spacer(Modifier.height(20.dp))
            Box(
                modifier = Modifier
                    .background(t.panelHi)
                    .border(1.dp, t.lineHi)
                    .clickable(onClick = onDismiss)
                    .padding(horizontal = 28.dp, vertical = 10.dp),
            ) {
                Text(
                    text = "OK",
                    color = t.ink,
                    fontSize = 11.sp,
                    letterSpacing = 0.24.em,
                    fontFamily = JetBrainsMono,
                )
            }
        }
    }
}

@Composable
private fun DeviceDetailsDialog(
    device: Device,
    onDismiss: () -> Unit,
    onConnect: () -> Unit,
) {
    val t = LocalTactical.current
    Dialog(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .background(t.panel)
                .border(1.dp, t.lineHi)
                .padding(24.dp)
                .widthIn(min = 320.dp),
        ) {
            Lbl(text = "SELECTED · ${device.id.uppercase()}")
            Spacer(Modifier.height(6.dp))
            Text(
                text = device.name.uppercase(),
                color = t.ink,
                fontSize = 22.sp,
                letterSpacing = 0.08.em,
                fontFamily = JetBrainsMono,
            )
            Spacer(Modifier.height(16.dp))

            DetailRow("LOCATION", device.location)
            DetailRow("STATUS", device.status.name)
            DetailRow("BATTERY", "${device.batteryLevel}%")
            DetailRow("IP", device.ipAddress)
            DetailRow("LATITUDE", String.format("%.6f", device.latitude))
            DetailRow("LONGITUDE", String.format("%.6f", device.longitude))

            Spacer(Modifier.height(20.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Box(
                    modifier = Modifier
                        .border(1.dp, t.line)
                        .clickable(onClick = onDismiss)
                        .padding(horizontal = 18.dp, vertical = 10.dp),
                ) {
                    Text("CANCEL", color = t.inkDim, fontSize = 11.sp, letterSpacing = 0.18.em, fontFamily = JetBrainsMono)
                }
                Box(
                    modifier = Modifier
                        .background(t.panelHi)
                        .border(1.dp, t.lineHi)
                        .clickable(onClick = onConnect)
                        .padding(horizontal = 18.dp, vertical = 10.dp),
                ) {
                    Text("CONNECT →", color = t.ink, fontSize = 11.sp, letterSpacing = 0.18.em, fontFamily = JetBrainsMono)
                }
            }
        }
    }
}

@Composable
private fun DetailRow(label: String, value: String) {
    val t = LocalTactical.current
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(label, color = t.inkDim, fontSize = 10.sp, letterSpacing = 0.18.em, fontFamily = JetBrainsMono)
        Text(value, color = t.ink, fontSize = 11.sp, fontFamily = JetBrainsMono)
    }
}
