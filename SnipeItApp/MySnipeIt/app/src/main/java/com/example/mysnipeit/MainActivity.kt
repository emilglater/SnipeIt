package com.example.mysnipeit

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.mysnipeit.ui.theme.MySniperItTheme
import com.example.mysnipeit.viewmodel.SniperViewModel
import com.example.mysnipeit.viewmodel.AppScreen
import com.example.mysnipeit.ui.home.HomeScreen
import com.example.mysnipeit.ui.device.DeviceSelectionScreen
import com.example.mysnipeit.ui.map.MapScreen
import com.example.mysnipeit.ui.dashboard.DashboardScreen
import android.util.Log
import com.example.mysnipeit.data.models.compassHeadingDeg
import com.example.mysnipeit.ui.dashboard.CalibrateBearingDialog
import com.example.mysnipeit.ui.dashboard.LoadoutDialog
import com.example.mysnipeit.ui.diagnostics.DiagnosticsScreen

class MainActivity : ComponentActivity() {
    private val viewModel: SniperViewModel by viewModels()

    // Runtime permission launcher for ACCESS_FINE_LOCATION. The actual
    // GPS provider lives in SniperViewModel; we just gate its start() on
    // permission grant here.
    private val locationPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) viewModel.onLocationPermissionGranted()
            else         viewModel.onLocationPermissionDenied()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableImmersiveMode()
        ensureLocationPermission()
        setContent {
            val darkTheme by viewModel.darkTheme.collectAsStateWithLifecycle()
            MySniperItTheme(darkTheme = darkTheme) {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    SniperApp(viewModel = viewModel)
                }
            }
        }
    }

    /**
     * If the location permission is already granted, start the provider now.
     * Otherwise launch the runtime request — the launcher's callback above
     * will start it on grant. If the user denies, [userLocation] stays null
     * forever and the map / future ballistics will degrade gracefully.
     */
    private fun ensureLocationPermission() {
        val granted = ContextCompat.checkSelfPermission(
            this, Manifest.permission.ACCESS_FINE_LOCATION
        ) == PackageManager.PERMISSION_GRANTED

        if (granted) {
            viewModel.onLocationPermissionGranted()
        } else {
            locationPermissionLauncher.launch(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // Re-hide system bars whenever the app regains focus (user swipes from
        // edge to peek the bars, then they auto-hide again on focus return).
        if (hasFocus) enableImmersiveMode()
    }

    /**
     * Hide both status bar and navigation bar (the bottom strip with the
     * Recents/Home/Back icons) and let users peek them with an edge swipe.
     */
    private fun enableImmersiveMode() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }
}

@Composable
fun SniperApp(viewModel: SniperViewModel) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val availableDevices by viewModel.availableDevices.collectAsStateWithLifecycle()
    // Raw stream — drives the Diagnostics LIVE SENSORS pane (operator sees
    // actual valid flags from the Pi). Dashboard uses the latched version.
    val sensorData by viewModel.sensorData.collectAsStateWithLifecycle()
    val latchedSensorData by viewModel.latchedSensorData.collectAsStateWithLifecycle()
    val sensorHistory by viewModel.sensorHistory.collectAsStateWithLifecycle()
    val acousticEvent by viewModel.acousticEvent.collectAsStateWithLifecycle()
    val activeAudioAlert by viewModel.activeAudioAlert.collectAsStateWithLifecycle()
    val detectedTargets by viewModel.detectedTargets.collectAsStateWithLifecycle()
    val firingSolution by viewModel.firingSolution.collectAsStateWithLifecycle()
    val systemStatus by viewModel.systemStatus.collectAsStateWithLifecycle()
    val selectedTargetId = uiState.selectedTargetId
    val streamReady by viewModel.streamReady.collectAsState()
    val rtspStreamUrl by viewModel.rtspStreamUrl.collectAsState()
    val darkTheme by viewModel.darkTheme.collectAsStateWithLifecycle()

    // Real device GPS — null until the user grants the runtime permission
    // and the FusedLocationProvider returns its first fix. Screens that
    // accept it handle the null case (MapScreen hides the marker; future
    // ballistics will use it directly as a calculator input).
    val userLocation by viewModel.userLocation.collectAsStateWithLifecycle()

    // Dashboard MENU dialog state lives in the ViewModel so it survives a
    // round-trip into Diagnostics (operator hits MENU → Diagnostics → BACK
    // and the menu reopens automatically). Loadout stays local — there's
    // no navigation away from it.
    val showMenu = uiState.dashboardMenuOpen
    var showLoadout by remember { mutableStateOf(false) }
    var showCalibrate by remember { mutableStateOf(false) }

    // Ballistic loadout — persisted cartridge + rifle profile picks; chosen
    // via MENU → Loadout on the dashboard, consumed by the ballistic solver.
    val selectedCartridge by viewModel.selectedCartridge.collectAsStateWithLifecycle()
    val selectedRifle by viewModel.selectedRifle.collectAsStateWithLifecycle()

    // Tripod world bearing — operator-calibrated offset that converts mic-
    // frame acoustic-event bearings into true-north world bearings. Saved
    // via MENU → Calibrate Bearing.
    val tripodWorldBearingDeg by viewModel.tripodWorldBearingDeg.collectAsStateWithLifecycle()
    val tripodCalibratedAtMs by viewModel.tripodCalibratedAtMs.collectAsStateWithLifecycle()

    // Diagnostics → MOCK MODE toggle. When ON the app streams synthetic
    // sensor data anchored to the operator's GPS so the ballistic
    // calculator can be tested without a Pi.
    val forceMockMode by viewModel.forceMockMode.collectAsStateWithLifecycle()

    // RTSP transport (TCP/UDP) debug toggle — set in Diagnostics → STREAM,
    // consumed by the dashboard's video player.
    val rtspForceTcp by viewModel.rtspForceTcp.collectAsStateWithLifecycle()

    when (uiState.currentScreen) {
        AppScreen.HOME -> {
            HomeScreen(
                onDeviceListClick = { viewModel.navigateToDeviceList() },
                onMapClick = { viewModel.navigateToMap() },
                onDiagnosticsClick = { viewModel.navigateToDiagnostics() },
                isDarkTheme = darkTheme,
                onToggleTheme = { viewModel.toggleTheme() },
            )
        }

        AppScreen.DEVICE_SELECTION -> {
            DeviceSelectionScreen(
                devices = availableDevices,
                onDeviceSelected = { device -> viewModel.selectDevice(device) },
                onBackClick = { viewModel.navigateToHome() },
                onMapClick = { viewModel.navigateToMap() },
                onDiagnosticsClick = { viewModel.navigateToDiagnostics() },
                isDarkTheme = darkTheme,
                onToggleTheme = { viewModel.toggleTheme() },
            )
        }

        AppScreen.MAP -> {
            MapScreen(
                devices = availableDevices,
                userLocation = userLocation,
                onDeviceSelected = { device -> viewModel.selectDevice(device) },
                onBackClick = { viewModel.navigateToHome() },
                isDarkTheme = darkTheme,
                onToggleTheme = { viewModel.toggleTheme() },
            )
        }

        AppScreen.DASHBOARD -> {
            DashboardScreen(
                sensorData = latchedSensorData,
                detectedTargets = detectedTargets,
                firingSolution = firingSolution,
                systemStatus = systemStatus,
                selectedTargetId = selectedTargetId,
                streamReady = streamReady,
                rtspStreamUrl = rtspStreamUrl,
                audioAlert = activeAudioAlert,
                audioAlertTimeoutMs = viewModel.audioAlertTimeoutMs,
                // SLEW returns the world bearing; step 5 will use it to
                // send an HTTP command to the Pi. For now we just clear
                // the alert (acceptAudioAlert already does that).
                onAudioAlertAccept = { viewModel.acceptAudioAlert() },
                onAudioAlertDismiss = { viewModel.dismissAudioAlert() },
                tripodCalibratedAtMs = tripodCalibratedAtMs,
                tripodCalibrationTimeoutMs = viewModel.tripodCalibrationTimeoutMs,
                rtspForceTcp = rtspForceTcp,
                onTargetSelect = { targetId ->
                    if (targetId.isEmpty()) viewModel.deselectTarget()
                    else viewModel.selectTarget(targetId)
                },
                onTargetLockToggle = { targetId, isLocking ->
                    if (isLocking) viewModel.lockTarget(targetId)
                    else viewModel.unlockTarget(targetId)
                },
                onConnectClick = { viewModel.connectToSystem() },
                onDisconnectClick = { viewModel.disconnectFromSystem() },
                onBackClick = { viewModel.goBackFromDashboard() },
                onMenuClick = { viewModel.setDashboardMenuOpen(true) },
                isDarkTheme = darkTheme,
                onToggleTheme = { viewModel.toggleTheme() },
            )

            // Menu dropdown — same as before; gives the dashboard a way to
            // reach Diagnostics and the Loadout picker without leaving the
            // operator surface.
            if (showMenu) {
                DashboardMenu(
                    onDismiss = { viewModel.setDashboardMenuOpen(false) },
                    onDiagnosticsClick = {
                        // Leave the menu OPEN in state — when the operator
                        // returns from Diagnostics the dashboard re-renders
                        // and the menu reopens automatically.
                        viewModel.navigateToDiagnostics()
                    },
                    onLoadoutClick = {
                        viewModel.setDashboardMenuOpen(false)
                        showLoadout = true
                    },
                    onCalibrateClick = {
                        viewModel.setDashboardMenuOpen(false)
                        showCalibrate = true
                    },
                )
            }

            if (showLoadout) {
                LoadoutDialog(
                    selectedCartridgeId = selectedCartridge.id,
                    selectedRifleId = selectedRifle.id,
                    onCartridgeSelect = { viewModel.selectCartridge(it) },
                    onRifleSelect = { viewModel.selectRifle(it) },
                    onDismiss = { showLoadout = false },
                )
            }

            if (showCalibrate) {
                CalibrateBearingDialog(
                    liveCompassDeg = latchedSensorData.compassHeadingDeg(),
                    currentCalibrationDeg = tripodWorldBearingDeg,
                    currentCalibratedAtMs = tripodCalibratedAtMs,
                    calibrationTimeoutMs = viewModel.tripodCalibrationTimeoutMs,
                    onCapture = { viewModel.calibrateTripodWorldBearing() },
                    onDismiss = { showCalibrate = false },
                )
            }
        }

        AppScreen.DIAGNOSTICS -> {
            DiagnosticsScreen(
                onBackClick = { viewModel.goBackFromDiagnostics() },
                sensorData = sensorData,
                sensorHistory = sensorHistory,
                acousticEvent = acousticEvent,
                forceMockMode = forceMockMode,
                onForceMockModeChange = { viewModel.setForceMockMode(it) },
                rtspForceTcp = rtspForceTcp,
                onRtspForceTcpChange = { viewModel.setRtspForceTcp(it) },
                isDarkTheme = darkTheme,
                onToggleTheme = { viewModel.toggleTheme() },
            )
        }
    }
}

@Composable
fun DashboardMenu(
    onDismiss: () -> Unit,
    onDiagnosticsClick: () -> Unit,
    onLoadoutClick: () -> Unit,
    onCalibrateClick: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Text(
                text = "Menu",
                style = MaterialTheme.typography.titleLarge
            )
        },
        text = {
            Column {
                MenuEntry(icon = "⚙ ", label = "Diagnostics", onClick = onDiagnosticsClick)
                MenuEntry(icon = "⌖ ", label = "Loadout", onClick = onLoadoutClick)
                MenuEntry(icon = "⏱ ", label = "Calibrate Bearing", onClick = onCalibrateClick)
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text("Close")
            }
        }
    )
}

@Composable
private fun MenuEntry(icon: String, label: String, onClick: () -> Unit) {
    TextButton(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth()
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.Start,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = icon,
                fontSize = 20.sp
            )
            Text(
                text = label,
                fontSize = 16.sp
            )
        }
    }
}