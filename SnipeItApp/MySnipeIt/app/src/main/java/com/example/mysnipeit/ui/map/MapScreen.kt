package com.example.mysnipeit.ui.map

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.example.mysnipeit.data.models.Device
import com.example.mysnipeit.data.models.DeviceStatus
import com.example.mysnipeit.ui.theme.Chip
import com.example.mysnipeit.ui.theme.ChipTone
import com.example.mysnipeit.ui.theme.JetBrainsMono
import com.example.mysnipeit.ui.theme.Lbl
import com.example.mysnipeit.ui.theme.LocalIsDarkTheme
import com.example.mysnipeit.ui.theme.LocalTactical
import com.example.mysnipeit.ui.theme.ThemeToggle
import com.example.mysnipeit.ui.theme.TopBar
import com.example.mysnipeit.ui.theme.responsiveDp
import com.example.mysnipeit.R
import com.google.android.gms.maps.model.BitmapDescriptorFactory
import com.google.android.gms.maps.model.CameraPosition
import com.google.android.gms.maps.model.LatLng
import com.google.android.gms.maps.model.MapStyleOptions
import com.google.maps.android.compose.GoogleMap
import com.google.maps.android.compose.MapProperties
import com.google.maps.android.compose.MapType
import com.google.maps.android.compose.MapUiSettings
import com.google.maps.android.compose.Marker
import com.google.maps.android.compose.MarkerState
import com.google.maps.android.compose.rememberCameraPositionState

/**
 * Map screen — design's "Sectors + intel pane" variant (Option B from
 * the chat), adapted: we keep Google Maps as the underlying terrain
 * surface (user's pick), apply a tactical dark style on top, and add
 * the design's right-hand 320dp intel pane.
 *
 * Markers use Google Maps' BitmapDescriptor in tactical colors — green
 * for ACTIVE, red for everything else — and the selected node fills the
 * intel pane with details + CONNECT button.
 */
@Composable
fun MapScreen(
    devices: List<Device>,
    userLocation: LatLng?,
    onDeviceSelected: (Device) -> Unit,
    onBackClick: () -> Unit,
    isDarkTheme: Boolean = true,
    onToggleTheme: () -> Unit = {},
) {
    val t = LocalTactical.current
    val isDark = LocalIsDarkTheme.current
    val context = LocalContext.current
    var selected by remember(devices) { mutableStateOf<Device?>(devices.firstOrNull { it.status == DeviceStatus.ACTIVE }) }

    // Initial camera center: prefer the user's real GPS, fall back to the
    // first ACTIVE device (gives operator-meaningful framing if GPS isn't
    // up yet), fall back finally to a sensible default near the demo area.
    val initialCenter: LatLng = userLocation
        ?: devices.firstOrNull { it.status == DeviceStatus.ACTIVE }
            ?.let { LatLng(it.latitude, it.longitude) }
        ?: LatLng(31.518209, 34.521274)

    val cameraState = rememberCameraPositionState {
        position = CameraPosition.fromLatLngZoom(initialCenter, 14f)
    }

    // Load the tactical dark map style. Only applied in dark mode; light
    // mode uses Google's default styling (which already reads well against
    // our khaki light background).
    val mapStyle = remember(isDark) {
        if (isDark) {
            runCatching {
                MapStyleOptions.loadRawResourceStyle(context, R.raw.maps_style_tactical_dark)
            }.getOrNull()
        } else null
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(t.base),
    ) {
        TopBar(
            device = "TACTICAL MAP",
            onBackClick = onBackClick,
        ) {
            ThemeToggle(isDark = isDarkTheme, onToggle = onToggleTheme)
        }
        Row(modifier = Modifier.fillMaxSize()) {
            // Left — Google Maps with the tactical dark style
            Box(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight(),
            ) {
                GoogleMap(
                    modifier = Modifier.fillMaxSize(),
                    cameraPositionState = cameraState,
                    properties = MapProperties(
                        // The "normal" type works much better with custom JSON
                        // styling than SATELLITE — satellite imagery would
                        // override the tactical colors.
                        mapType = MapType.NORMAL,
                        mapStyleOptions = mapStyle,
                        isMyLocationEnabled = false,
                    ),
                    uiSettings = MapUiSettings(
                        zoomControlsEnabled = false,
                        myLocationButtonEnabled = false,
                        compassEnabled = false,
                        mapToolbarEnabled = false,
                    ),
                ) {
                    // Only draw the "Your Location" marker once we actually
                    // have a fix. Until then the user sees only the device
                    // markers + the "GPS ACQUIRING…" chip in the map overlay.
                    userLocation?.let { loc ->
                        Marker(
                            state = MarkerState(position = loc),
                            title = "Your Location",
                            // Use a low-saturation hue. HUE_AZURE reads as a muted
                            // teal under the tactical style — far less attention-
                            // grabbing than a saturated blue.
                            icon = BitmapDescriptorFactory.defaultMarker(BitmapDescriptorFactory.HUE_AZURE),
                        )
                    }
                    devices.forEach { device ->
                        val position = LatLng(device.latitude, device.longitude)
                        val hue = when (device.status) {
                            DeviceStatus.ACTIVE -> BitmapDescriptorFactory.HUE_GREEN
                            DeviceStatus.SCANNING -> BitmapDescriptorFactory.HUE_YELLOW
                            DeviceStatus.INACTIVE,
                            DeviceStatus.ERROR -> BitmapDescriptorFactory.HUE_RED
                        }
                        Marker(
                            state = MarkerState(position = position),
                            title = device.name,
                            snippet = device.status.name,
                            icon = BitmapDescriptorFactory.defaultMarker(hue),
                            onClick = {
                                selected = device
                                true
                            },
                        )
                    }
                }
                // Tactical chrome overlay — coords badge + GPS-status pill.
                // BACK now lives in the TopBar so it's in the same place
                // across screens.
                MgrsBadge()
                GpsStatusBadge(hasFix = userLocation != null)
            }

            // Right — 320dp intel pane (always present, with placeholder copy
            // when nothing is selected)
            IntelPane(
                device = selected,
                onConnect = { selected?.let(onDeviceSelected) },
            )
        }
    }
}

// ----------------------------------------------------------------------------
// Map overlays
// ----------------------------------------------------------------------------
@Composable
private fun MgrsBadge() {
    val t = LocalTactical.current
    Column(
        modifier = Modifier
            .padding(12.dp)
            .background(t.panel.copy(alpha = 0.92f))
            .border(1.dp, t.line)
            .padding(horizontal = 10.dp, vertical = 8.dp),
    ) {
        Text(
            text = "TACTICAL MAP · GRID 33S",
            color = t.ink,
            fontSize = 10.sp,
            letterSpacing = 0.18.em,
            fontFamily = JetBrainsMono,
        )
        Spacer(Modifier.height(2.dp))
        Text(
            text = "ZOOM 14 · UTM",
            color = t.inkDim,
            fontSize = 9.sp,
            letterSpacing = 0.18.em,
            fontFamily = JetBrainsMono,
        )
    }
}

/**
 * Small chip in the top-right corner of the map that surfaces whether the
 * device GPS has produced a fix yet. Green dot = we have one (the "Your
 * Location" marker is drawn), amber dot = still acquiring (no marker).
 */
@Composable
private fun BoxScope.GpsStatusBadge(hasFix: Boolean) {
    val t = LocalTactical.current
    Row(
        modifier = Modifier
            .align(Alignment.TopEnd)
            .padding(12.dp)
            .background(t.panel.copy(alpha = 0.92f))
            .border(1.dp, t.line)
            .padding(horizontal = 10.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = "●",
            color = if (hasFix) t.on else t.accent,
            fontSize = 10.sp,
            fontFamily = JetBrainsMono,
        )
        Spacer(Modifier.width(6.dp))
        Text(
            text = if (hasFix) "GPS LOCK" else "GPS ACQUIRING…",
            color = t.ink,
            fontSize = 10.sp,
            letterSpacing = 0.18.em,
            fontFamily = JetBrainsMono,
        )
    }
}

// ----------------------------------------------------------------------------
// Intel pane — right side
// ----------------------------------------------------------------------------
@Composable
private fun IntelPane(
    device: Device?,
    onConnect: () -> Unit,
) {
    val t = LocalTactical.current
    Column(
        modifier = Modifier
            .width(responsiveDp(tablet = 320.dp, compact = 240.dp))
            .fillMaxHeight()
            .background(t.panel)
            .border(1.dp, t.line)
            .padding(horizontal = 22.dp, vertical = 20.dp)
            .verticalScroll(rememberScrollState()),
    ) {
        if (device == null) {
            Lbl(text = "NO NODE SELECTED")
            Spacer(Modifier.height(8.dp))
            Text(
                text = "Tap a marker on the map to view node details.",
                color = t.inkDim,
                fontSize = 11.sp,
                fontFamily = JetBrainsMono,
            )
            return@Column
        }

        Lbl(text = "SELECTED · ${device.id.uppercase()}")
        Spacer(Modifier.height(6.dp))
        Text(
            text = device.name.uppercase(),
            color = t.ink,
            fontSize = 22.sp,
            letterSpacing = 0.08.em,
            fontFamily = JetBrainsMono,
        )
        Spacer(Modifier.height(8.dp))
        val (statusText, tone) = when (device.status) {
            DeviceStatus.ACTIVE -> "ACTIVE" to ChipTone.On
            DeviceStatus.INACTIVE -> "OFFLINE" to ChipTone.Danger
            DeviceStatus.SCANNING -> "STANDBY" to ChipTone.Warn
            DeviceStatus.ERROR -> "ERROR" to ChipTone.Danger
        }
        Chip(text = statusText, tone = tone)

        Spacer(Modifier.height(22.dp))
        DividerLine()
        Spacer(Modifier.height(18.dp))

        // Two-column grid of the data we actually have on each Device
        IntelGrid(device = device)

        Spacer(Modifier.height(22.dp))
        DividerLine()
        Spacer(Modifier.height(18.dp))

        Lbl(text = "RECENT DETECTIONS")
        Spacer(Modifier.height(8.dp))
        Text(
            text = "—",
            color = t.inkDim,
            fontSize = 11.sp,
            fontFamily = JetBrainsMono,
        )

        Spacer(Modifier.weight(1f, fill = false))
        Spacer(Modifier.height(18.dp))
        // CONNECT button (only enabled for ACTIVE nodes)
        val enabled = device.status == DeviceStatus.ACTIVE
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .background(if (enabled) t.panelHi else Color.Transparent)
                .border(1.dp, if (enabled) t.lineHi else t.line)
                .clickable(enabled = enabled, onClick = onConnect)
                .padding(vertical = 14.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = if (enabled) "CONNECT →" else "NODE OFFLINE",
                color = if (enabled) t.ink else t.inkMute,
                fontSize = 11.sp,
                letterSpacing = 0.24.em,
                fontFamily = JetBrainsMono,
            )
        }
    }
}

@Composable
private fun IntelGrid(device: Device) {
    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(18.dp)) {
            IntelCell(label = "SECTOR", value = device.location.uppercase(), modifier = Modifier.weight(1f))
            IntelCell(label = "STATUS", value = device.status.name, modifier = Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(18.dp)) {
            IntelCell(label = "BATTERY", value = "${device.batteryLevel}%", modifier = Modifier.weight(1f))
            IntelCell(label = "IP", value = device.ipAddress, modifier = Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(18.dp)) {
            IntelCell(label = "LATITUDE",  value = String.format("%.5f", device.latitude),  modifier = Modifier.weight(1f))
            IntelCell(label = "LONGITUDE", value = String.format("%.5f", device.longitude), modifier = Modifier.weight(1f))
        }
    }
}

@Composable
private fun IntelCell(label: String, value: String, modifier: Modifier = Modifier) {
    val t = LocalTactical.current
    Column(modifier = modifier) {
        Lbl(text = label)
        Spacer(Modifier.height(4.dp))
        Text(
            text = value,
            color = t.ink,
            fontSize = 14.sp,
            letterSpacing = 0.06.em,
            fontFamily = JetBrainsMono,
        )
    }
}

@Composable
private fun DividerLine() {
    val t = LocalTactical.current
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(t.line)
    )
}
