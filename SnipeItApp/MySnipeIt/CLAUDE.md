# MySnipeIt — Project Context for Claude

> Onboarding doc for new Claude sessions. Read this first to skip exploration. Update on every meaningful commit (see "Maintenance" at the bottom).

## What this is

Android tactical operator app that talks to a **Raspberry Pi 5** mounted on a remote sniper/sensor rig. The phone/tablet is the operator HUD: it shows the Pi's live RTSP video, overlays ML target detections, displays sensor telemetry (rangefinder, temp/humidity, GPS, wind, servo angles, compass), and sends commands (lock/unlock target, calibrate, manual target, e-stop) back to the Pi over WebSocket + HTTP.

- **Platform:** Android, `minSdk 26`, `targetSdk/compileSdk 34`, landscape-only, immersive (system bars hidden).
- **UI:** 100% Jetpack Compose, Material3, custom "tactical" palette (dark + light).
- **Form factor:** Designed for 1280×800 tablet landscape; phone-landscape (<840dp wide) falls back to compact layouts via `Responsive.kt`.
- **Package:** `com.example.mysnipeit`
- **App name string:** `MySnipeIt`

## Tech stack

- **Language:** Kotlin 2.0.21, JVM target 1.8
- **Build:** Gradle Kotlin DSL, AGP 8.12.1, version catalog at `gradle/libs.versions.toml` (note: catalog is partially used — `app/build.gradle.kts` still hardcodes most deps directly)
- **UI:** Jetpack Compose (BOM `2023.10.01`), Material3, Navigation Compose 2.7.4, `kotlin-parcelize`, `kotlin.plugin.compose`
- **Architecture:** Single-Activity → `SniperViewModel` (AndroidViewModel) → `SniperRepository` → `RaspberryPiClient`. State via `StateFlow`, collected with `collectAsStateWithLifecycle`. No DI framework wired up yet (Hilt is in the version catalog but not applied).
- **Networking:** Retrofit 2.9 + Gson + OkHttp 4.12 (declared, but actual Pi comms use raw `HttpURLConnection` + `Java-WebSocket 1.5.3`). Gson is the JSON parser in use.
- **Video:** Media3 ExoPlayer 1.2.0 with RTSP source (`androidx.media3:media3-exoplayer-rtsp`). Legacy ExoPlayer 2.19.1 also present — prefer Media3 for new code.
- **Maps:** `play-services-maps` + `maps-compose` 4.3.0, plus `play-services-location` (FusedLocationProvider). Google Maps API key injected via Secrets Gradle plugin as `${MAPS_API_KEY}` in the manifest — put it in `local.properties` as `MAPS_API_KEY=...`.
- **Permissions runtime:** Accompanist Permissions 0.32.0. Location permission is requested manually in `MainActivity` (not via Accompanist there).
- **JSON:** Gson 2.10.1 (kotlinx-serialization is in the catalog but not applied).
- **Persistence:** `SharedPreferences` only ("snipeit" prefs: `dark_theme` boolean, `cartridge_id` + `rifle_id` strings for the ballistic loadout, `tripod_world_bearing_deg` float + `tripod_calibrated_at_ms` long for the operator-calibrated mic-array world-bearing offset and its capture timestamp, `rtsp_force_tcp` boolean for the RTSP transport toggle). No Room, no DataStore.

## Repo layout

```
MySnipeIt/
├── app/
│   ├── build.gradle.kts            # All app deps (mostly hardcoded, not via catalog)
│   ├── proguard-rules.pro
│   └── src/main/
│       ├── AndroidManifest.xml     # Permissions, MAPS_API_KEY, landscape-only MainActivity
│       └── java/com/example/mysnipeit/
│           ├── MainActivity.kt                       # Single Activity, hosts Compose root, perms, immersive mode
│           ├── viewmodel/SniperViewModel.kt          # All app state, nav, device list, theme toggle
│           ├── data/
│           │   ├── ballistics/
│           │   │   ├── TargetLocalizer.kt             # Pure fn: Pi sensors → target world coords; RigGeometry constants
│           │   │   └── FiringSolutionSolver.kt        # Pure fn: sniper GPS + target + cartridge/rifle/atmosphere → AZ / hold-over / windage / TOF
│           │   ├── location/DeviceLocationProvider.kt # FusedLocationProvider wrapper → StateFlow<LatLng?>
│           │   ├── models/                            # All data classes (Device, SensorData, Target, SystemStatus, BallisticProfiles)
│           │   ├── network/
│           │   │   ├── RaspberryPiClient.kt          # WS + HTTP client to RPi; pass-through detections (Pi owns tracking) + keepalive
│           │   │   ├── WifiBinder.kt                 # Force traffic onto WiFi (RPi AP has no internet)
│           │   │   ├── WifiPerfLock.kt               # FULL_LOW_LATENCY WifiLock + wake lock held during the live view (suppresses WiFi power-save)
│           │   │   └── Networktester.kt              # TCP-based ping/port-scan utility (currently unused, kept for diag)
│           │   └── repository/SniperRepository.kt    # Thin pass-through over RaspberryPiClient
│           └── ui/
│               ├── home/HomeScreen.kt                # Landing "secure terminal"
│               ├── device/DeviceSelectionScreen.kt   # List of 4 hard-coded devices
│               ├── map/MapScreen.kt                  # Google Map w/ device pins + user location
│               ├── dashboard/
│               │   ├── DashboardScreen.kt            # Operator HUD chrome around the video
│               │   ├── TacticalVideoPlayer.kt        # ExoPlayer RTSP + bbox overlays + reticles
│               │   ├── LoadoutDialog.kt              # Cartridge + rifle profile picker (MENU → Loadout)
│               │   └── MockVideoFeed.kt              # Plays bundled field_video.mp4 when no RTSP
│               ├── diagnostics/DiagnosticsScreen.kt  # Live sensor / status dump
│               └── theme/
│                   ├── Theme.kt                      # MySniperItTheme; exposes LocalTactical + LocalIsDarkTheme
│                   ├── Color.kt                      # TacticalDark + TacticalLight palettes (+ deprecated aliases)
│                   ├── Type.kt                       # Inter + JetBrains Mono fonts
│                   ├── TacticalComponents.kt         # Shared Chip, TopBar, Bracket button, etc.
│                   └── Responsive.kt                 # isCompactWidth() / responsiveDp() — 840dp breakpoint
│       └── res/
│           ├── raw/field_video.mp4                   # Demo video for MockVideoFeed
│           ├── raw/maps_style_tactical_dark.json     # Dark map style
│           ├── font/                                 # Inter + JetBrains Mono ttf files
│           └── ... (standard mipmap/values/xml)
├── build.gradle.kts                 # Root plugins (apply false)
├── settings.gradle.kts              # rootProject = "MySnipeIt", includes :app
└── gradle/libs.versions.toml        # Version catalog (partially used)
```

## Architecture & data flow

```
RPi5 ──WS:8555──► RaspberryPiClient ──StateFlow──► SniperRepository ──StateFlow──► SniperViewModel ──collectAsState──► Composables
     ◄──WS:8555── (commands: select_target lock/unlock, set_servo_angles slew — via sendWsCommand)
     ──HTTP:8000◄── (calibrate, manual target, emergency_stop — Pi has no HTTP server yet, no-ops)
     ──RTSP:8554──► TacticalVideoPlayer (ExoPlayer RTSP)
```

### Key state flows (read these in any new screen)

Exposed from `SniperViewModel`:
- `uiState: StateFlow<SniperUiState>` — `currentScreen`, `selectedDeviceId`, `selectedTargetId`, `previousScreen`, etc.
- `availableDevices` — **hardcoded list of 4 devices** (Device 1–4) with fake GPS coords in the Negev region. Device 3 uses `WifiBinder.FALLBACK_GATEWAY` (`10.42.1.1`) as its IP — this is the real RPi.
- `userAltitudeM: StateFlow<Double?>` — device altitude in metres MSL when the GPS fix has a vertical component; null otherwise. Fed into the firing-solution solver as the sniper's elevation.
- `firingSolution: StateFlow<FiringSolution?>` — app-computed solution from `latchedSensorData` + `userLocation`/`userAltitudeM` + selected cartridge/rifle. Recomputes reactively via `combine()`. **The Pi does NOT compute or send a firing solution** — by design it publishes only the raw sensor data needed to compute one, because the solution depends on where the SHOOTER stands and the rig can't know that. This is the only firing solution in the system.
- `sensorData: StateFlow<SensorData?>` — RAW stream straight from the Pi; consumed by the Diagnostics LIVE SENSORS pane so the operator sees actual valid flags.
- `latchedSensorData: StateFlow<SensorData?>` — sticky version of `sensorData`: each sub-frame holds its last VALID reading for up to `sensorLatchTimeoutMs` (default 5 s; tunable on `SniperViewModel`) before falling back to "—". Dashboard consumes this. Wind speed + direction latch independently (two valid flags), the compass only latches when `heading_deg` is non-null so a missing heading is never substituted as `0°`, and the servo only latches when BOTH angles are non-null — same reason, plus a partial frame must not displace the last good one (the acoustic slew reads the latched `verticalDeg` to hold the tilt).
- `sensorHistory: StateFlow<List<SensorData>>` — rolling window of the last 10 raw frames (oldest first). Powers the Diagnostics LIVE SENSORS history strip.
- `acousticEvent: StateFlow<AcousticEvent?>` — latest TDOA detection from the Pi's 4-mic module (separate WS message, not part of `ddl_frame`). Single-slot — dedupe/timeout/alert state is in `activeAudioAlert` below.
- `tripodWorldBearingDeg: StateFlow<Double?>` — operator-calibrated world bearing of the tripod-forward direction (= mic 0° axis world bearing). Persisted in SharedPreferences. Set via dashboard MENU → Calibrate Bearing (`CalibrateBearingDialog`) when the operator centres the camera at servo 90° — the latched compass at that moment IS this value (since the compass is on the moving head). Null when never calibrated. The saved value also **expires** after `tripodCalibrationTimeoutMs` (default 90 min, tunable) — `isCalibrationValid()` / `effectiveTripodWorldBearingDeg()` gate its USE (the value isn't wiped, just ignored once stale). When uncalibrated OR expired, audio alerts still fire but show the RELATIVE mic angle instead of a world bearing (the SLEW still works — it only needs the raw azimuth).
- `tripodCalibratedAtMs: StateFlow<Long?>` — wall-clock timestamp of the most recent successful calibration. Drives the "Last calibrated: X ago" line in the dialog and the `CalibrationAgeChip` in the dashboard top bar (tone bands: < 30 min On, 30 min–timeout Warn, ≥ timeout Danger + "CAL EXP"). The red band coincides with `tripodCalibrationTimeoutMs` — chip-red == expired.
- `activeAudioAlert: StateFlow<AudioAlert?>` — derived alert state for the dashboard. Built from `acousticEvent` + `effectiveTripodWorldBearingDeg()` + `uiState.selectedTargetId`. Computes the world bearing via `worldBearingFromAcousticEvent` when the calibration is valid (sets `AudioAlert.isWorldBearing=true`); otherwise the alert carries the raw mic azimuth and `isWorldBearing=false` (UI shows "REL <angle>"). Dedupe/debounce key on the **raw mic azimuth** (always present, calibration-independent): dedupes same-source events within ±15° (refreshing in place), auto-dismisses 20 s after the FIRST event of a dedupe group (`audioAlertTimeoutMs`, tunable), debounces re-fires for 30 s after explicit DISMISS, and flips `isInteractive` based on whether a target is selected (locked → passive chip, unlocked → full card). Setters: `acceptAudioAlert()` (sends the slew via raw azimuth) and `dismissAudioAlert()` (arms the debounce).
- `detectedTargets: StateFlow<List<DetectedTarget>>` — the Pi's detections published verbatim (stable Pi track ids, per-detection `confirmed` flag). No app-side tracking.
- `systemStatus: StateFlow<SystemStatus>`
- `streamReady: StateFlow<Boolean>` + `rtspStreamUrl: StateFlow<String?>`
- `userLocation: StateFlow<LatLng?>` — device GPS, null until permission granted + first fix
- `darkTheme: StateFlow<Boolean>` — persisted to SharedPreferences
- `selectedCartridge: StateFlow<CartridgeProfile>` + `selectedRifle: StateFlow<RifleProfile>` — ballistic loadout, persisted to SharedPreferences, chosen via dashboard MENU → Loadout (`LoadoutDialog`). Presets live in `BallisticProfiles`.
- `forceMockMode: StateFlow<Boolean>` — operator-triggered offline test path; setter `setForceMockMode(true)` disconnects from any real Pi and starts the in-app mock generator. Wired to Diagnostics → MOCK MODE. The mock anchors the synthetic Pi ~100 m east of the operator's own GPS so the ballistic calculator stays in range regardless of where the device is.

### Navigation

No Nav Compose graph — `SniperApp` does a manual `when (uiState.currentScreen)` switch over an `AppScreen` enum: `HOME → DEVICE_SELECTION/MAP → DASHBOARD → DIAGNOSTICS`. ViewModel exposes `navigateToX()` functions. `goBackFromDashboard()` uses `previousScreen` to return to whichever entry path was used (map vs device list).

## The Pi protocol (critical)

`RaspberryPiClient` is where almost all integration complexity lives. Read this whole file before touching networking code. **It has a long JSDoc footer at the bottom showing exact JSON shapes — do not invent shapes.**

### Ports
- **WebSocket:** `ws://<ip>:8555` — bidirectional. Inbound (Pi→app): sensor data, target detections, shooting solutions, `acoustic_event`, `stream_ready`, system status. Outbound (app→Pi): commands via `RaspberryPiClient.sendWsCommand()` with envelope `{type:"command", command, params, timestamp}`. The Pi parses these in its WS receive handler (`websocket_server.c` → `ddl_bridge_handle_command`) and dispatches to the servo event bus. Outbound commands: `select_target` (lock/unlock → servo LOCK / SCAN events) and `set_servo_angles` (acoustic slew → `ddl_servo_set_target` + NOISE_DETECTED event, which slews + resumes the autonomous scan). **`set_servo_angles` always carries BOTH `horizontal_deg` and `vertical_deg`** — there's no "leave this axis alone" encoding, so a horizontal-only slew holds the tilt by echoing the camera's current `vertical_deg` back (see the slew gotcha below).
- **HTTP commands:** `POST http://<ip>:8000/api/command` with `{command, params}` — `RaspberryPiClient.sendCommand()`. **The Pi has NO HTTP server**, so these are currently no-ops (`calibrate_system`, `set_manual_target`, `emergency_stop` from `SniperRepository` go here and land nowhere). Kept for when/if the Pi adds an HTTP server; all commands that actually need to work go over the WS instead.
- **RTSP video:** `rtsp://<ip>:8554/<stream_name>` (stream name comes from `stream_ready` event)

### Incoming WS message types (handled in `handleWebSocketMessage`)
- `sensor_data` — nested `ddl_frame` with `distance` / `temperature_humidity` / `servo` / `gps` / `compass` / `wind` sub-frames. Each sub-frame has a `valid` flag (except `servo`, which has none, and `wind`, which has two: `speed_valid` and `direction_valid` independently). Dashboard hides values when `valid=false`. Wind speed + direction are shown on the bottom sensor strip; compass + servo are parsed but NOT displayed (kept for the future ballistics calculator). See `SensorData.kt` for the helper extensions (`distanceM()`, `gpsLatLon()`, `windSpeedMps()`, `windDirectionDeg()`, `compassHeadingDeg()`, etc.) — always use them, don't access nested fields directly.
- `target_detection` — `{timestamp_ms, detections:[{id, class, confidence, bbox{x,y,width,height}, confirmed}]}`. `bbox` is pixels against a **1920×1080** video frame (hardcoded `VIDEO_WIDTH/VIDEO_HEIGHT` in `TacticalVideoPlayer.kt`). `id` is a stable Pi track id, `confirmed` gates lockability (absent = fallback/overlay-only). Empty `detections` = clear. See "Detection pipeline" below.
- `stream_ready` — `{rtsp_port, stream_name}` → builds `rtspStreamUrl` and flips `streamReady`.
- `system_status` — direct deserialize into `SystemStatus`.
- `acoustic_event` — `{type, timestamp_us, azimuth_deg, confidence, peak_amplitude, duration_ms, valid}`. Single TDOA detection from the Pi's 4-mic module. `azimuth_deg` is in the **mic-array's own frame** (the array is bolted to the fixed tripod, doesn't move with the servos). World bearing is computed via `worldBearingFromAcousticEvent(event, tripodWorldBearingDeg)` where the second argument is the operator-calibrated `tripodWorldBearingDeg` (NOT the live compass — the compass is on the moving head, so it can't tell us where the mic array is pointing once the head has moved). Until calibration runs (or after it expires), the world bearing is null and the alert shows the RELATIVE mic angle instead — the alert still fires and SLEW still works (slew only needs the raw azimuth + 90° → servo).

### Detection pipeline — the Pi owns tracking now

**The Pi runs a motion-compensated tracker** (Jetson Orin does stateless per-frame inference; the Pi joins detections to servo pose, associates in world-angle space so IDs survive pans, and coasts tracks ≤1.5 s through gaps). So the app is a **pass-through display** — there is deliberately NO app-side tracker/re-ID/smoothing/coasting anymore (removed on the `detection-contract` branch; it used to fight the Pi's tracker → on-screen ID churn).

Flow now:
1. `target_detection` message → parsed in `handleWebSocketMessage` and published **verbatim** to `_detectedTargets` (no channel, no pacer). Fields: `id` (**stable Pi track id** — render as-is, send back on lock), `class`, `confidence`, `bbox`, and optional `confirmed`.
2. **`confirmed` per detection** — `true` once a track has ≥2 hits; the Pi only locks confirmed tracks. UI ghosts unconfirmed boxes (dim alpha) and shows "UNCONFIRMED" instead of a LOCK button. **`confirmed` ABSENT (parsed as `null`) = fallback/overlay-only** message (raw per-frame Orin ids) → nothing in it is lockable. Detect fallback by null, not by id values.
3. **Empty `detections: []` clears the overlay immediately** — the Pi signals "clear all boxes" explicitly.
4. **Staleness watchdog (`startStalenessChecker`)** — LINK-LOST BACKSTOP ONLY: wipes the overlay if NO message arrives for `STALENESS_TIMEOUT_MS = 3s`. Normal clearing is the empty array (step 3), not a timeout.
5. **WS keepalive** — pings every `20s` to prevent NAT idle timeout. Unknown message types fall through silently on the C server.

**`timestamp_ms` is the Orin's monotonic clock** — NOT epoch, NOT comparable to the app's clock. The app does not read it (staleness uses the app's own receive time).

**Lock sends the wire `id` verbatim** — because the app now displays the wire id, `selectedTargetId` IS the wire id, so `select_target.target_id` round-trips correctly. (Pre-`detection-contract`, the app sent its own `T1/T2` id → the Pi couldn't match it → fell back to highest-confidence = "locked the wrong target".)

Bbox tweening is currently **stripped** — boxes snap straight to the Pi's reported position so the outdoor validation run sees the tracker's raw motion. Pure-visual tweening (glide between reported positions, keyed on the wire `id`, no id/lifecycle change) is permitted and can be re-added after the outdoor run.

### Mock fallback

If `connectWebSocket` throws OR `connect()` itself fails, `startMockDataGeneration()` runs a 1.5s tick that fakes sensor data + 2 targets (ids `1` HUMAN, `2` DRONE, both `confirmed=true`) + a random shooting solution. Useful for UI dev without the Pi.

There's also a `MockVideoFeed` that plays `res/raw/field_video.mp4` when no RTSP is available.

### WiFi binding (`WifiBinder`)

The RPi5 AP "MyHotspot" has **no internet upstream**. Android prefers cellular when WiFi has no internet, so all RPi sockets get routed to cellular and fail. `WifiBinder.bindToWifi()` requests a WiFi network with `removeCapability(NET_CAPABILITY_INTERNET)` and binds the process to it. `getGatewayIp()` reads the DHCP gateway (little-endian int → IPv4 string), falling back to `10.42.1.1`. `release()` is called on disconnect and in `onCleared()`.

## UI conventions

- **Palette access:** `val t = LocalTactical.current` at the top of every composable. Never hardcode colors. Use `t.base / t.panel / t.ink / t.accent / t.on / t.danger / t.bboxTracked / t.bboxLocked` etc.
- **Dark/light awareness:** `LocalIsDarkTheme.current` for non-palette decisions (e.g. picking dark vs light Google Maps JSON).
- **Bbox colors:** `bboxTracked` (orange) and `bboxLocked` (red) are intentionally **identical hex in both palettes** because they sit over the live video, not the UI.
- **Typography:** Use `JetBrainsMono` for tactical/monospace text (HUD chips, status), Inter for body. Both exposed from `theme/Type.kt`.
- **Shared components:** `TacticalComponents.kt` exports `TopBar`, `Chip`, `ChipTone`, `Bracket` button, `Lbl`, `ThemeToggle`, `TopBarIconButton`. Reuse these — don't rebuild them per screen.
- **Compact layout:** Use `isCompactWidth()` / `responsiveDp(tablet, compact)` to branch on phone-landscape (<840dp). Default reference size is 1280×800.
- **Deprecated color aliases:** `MilitaryDarkBackground`, `StatusConnected`, etc. in `Color.kt` are legacy — do not use in new code, and prefer migrating any usage you touch.
- **HUD bars that overlay the video must declare their height to the player.** `DashboardScreen` draws the bottom sensor strip *over* the video region, so it passes `SensorStripHeight` into `TacticalVideoPlayer` as `bottomOverlayObstruction` (`topOverlayObstruction` is `0.dp` — the TopBar stacks above the video rather than overlaying it). The player subtracts the letterbox slack, and keeps every bbox info card out of what remains. Skip this and a card's LOCK button ends up visible but not pressable underneath the strip. Any new video-overlaying bar needs the same treatment.
- **Bbox interaction:** double-tap a bbox to lock/unlock (same guard as the card's LOCK button); single-tap is wired to `onTargetClick`, currently a no-op at the call site. The gesture area expands symmetrically around the drawn box to a minimum 48 dp (`MIN_TAP_TARGET`) so distant targets stay pressable — the *drawn* rectangle stays at the true bbox size. Handlers go through `rememberUpdatedState` because `pointerInput` is keyed on `Unit` and would otherwise capture stale closures.
- **The bbox info card is a SIBLING of the bbox box, not a child** — a child would be size-constrained by narrow bboxes and wrap its text. It measures itself via `onSizeChanged` and stays at `alpha 0` for the first frame until that lands.

## Permissions (manifest)

`INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`, `CHANGE_WIFI_STATE`, `CHANGE_NETWORK_STATE`, `ACCESS_FINE_LOCATION`, `ACCESS_COARSE_LOCATION`, `RECORD_AUDIO`, `WAKE_LOCK`, `VIBRATE`. Cleartext traffic enabled (`usesCleartextTraffic="true"`) — required for the RPi's plain HTTP/RTSP.

Runtime permission flow: `MainActivity.ensureLocationPermission()` checks `ACCESS_FINE_LOCATION`; on grant it calls `viewModel.onLocationPermissionGranted()` which starts the `DeviceLocationProvider`. Deny path leaves `userLocation` null forever (map hides marker; future ballistics degrades gracefully).

## Build / run

- Open in Android Studio (project uses AGP 8.12.1 → Studio Hedgehog+ or newer).
- Add `MAPS_API_KEY=AIza...` to `local.properties` (gitignored).
- Run on a landscape Android 8.0+ device. Phone works but tablet is the target.
- For RPi testing: phone must be joined to the RPi's WiFi AP (`MyHotspot`, gateway `10.42.1.1`). WifiBinder handles routing.
- For UI-only dev: just run and ignore connection errors — mock data kicks in automatically after a few seconds.

## Known gotchas

- **No `versionCatalog` discipline** — `app/build.gradle.kts` hardcodes most versions instead of using `libs.versions.toml`. Catalog has Hilt/serialization entries that are unused. If you migrate to catalog, also remove the duplicates from `app/build.gradle.kts`.
- **Two ExoPlayer generations coexist** — `com.google.android.exoplayer:exoplayer 2.19.1` AND `androidx.media3:media3-exoplayer 1.2.0`. New code should use Media3.
- **`testHttpApi` and `NetworkTester` are unused** — kept around for future diagnostic UI work. The commented block in `RaspberryPiClient.connect()` shows the original gated-by-port-scan flow.
- **`SystemStatus` from WS doesn't match `SystemStatus` data class precisely** — Gson parses field-by-field, missing fields become defaults. If you add fields, double-check both sides.
- **Hardcoded video resolution** — 1920×1080 in `TacticalVideoPlayer.kt`. If the Pi ever changes resolution, this breaks bbox scaling.
- **Stream resilience lives in `TacticalVideoPlayer.kt`** — the RTSP session wedges after the Pi soft-AP's periodic Wi-Fi blackouts (frames stop even though transport recovers) and ExoPlayer won't self-heal. A stall watchdog polls playback progress every 500 ms and, if it hasn't advanced for `STALL_TIMEOUT_MS` (3 s) while playing, rebuilds the RTSP source (`buildRtspMediaSource`) with exponential backoff — doing programmatically what a manual back-out-and-reconnect does, and resetting accumulated latency. RTSP transport (RTP-over-TCP vs UDP) is an operator A/B toggle — Diagnostics → STREAM, persisted as `rtsp_force_tcp` (default TCP), flipping it reloads the live stream. TCP matches the Pi's `-rtsp_transport tcp` (reliable, stalls on loss); UDP degrades gracefully on a lossy link (needs the Pi to accept UDP). `WifiPerfLock` (FULL_LOW_LATENCY WifiLock + wake lock) is held for the life of the player to fight client-side Wi-Fi power-save. **Video is decoupled from the WS control channel**: a WS close no longer clears `rtspStreamUrl`/`streamReady` (only an explicit `disconnect()` does), and the player loads/watches on `streamReady` alone — so a control-channel blip can't tear down video. The dashboard shows two separate chips: **LINK** (WS/control state) and **VIDEO** (driven by real frame flow via the player's `onVideoHealthChanged` callback, not WS state).
- **`previousScreen` nav is a hack** — manual back-stack tracking instead of Nav Compose. Tolerable for 5 screens, would need replacing if nav gets richer.
- **Few tests, all pure-math** — `ExampleInstrumentedTest`/`ExampleUnitTest` are unmodified AS templates. The three real suites all live under `app/src/test/.../data/ballistics/`: `TargetLocalizerTest` (geodesy for the localizer), `FiringSolutionSolverTest` (solver + drag table + downhill regression cover), `AcousticBearingTest` (mic azimuth → world bearing). Nothing covers the UI, the ViewModel or `RaspberryPiClient`. **Test names must not contain `->`** — backtick-quoted JVM test names reject it and fail to compile; write "gives" instead.
- **`RigGeometry` constants reflect the real rig** — `COMPASS_ON_FIXED_BASE = false` (the compass is bolted to the moving camera arm, so its reading IS the camera's pointing direction — servo pan is NOT added on top), servo pan/tilt centered at 90°, declination 0, and `MIC_TO_SERVO_OFFSET_DEG = 90.0` (mic↔servo, used for the SLEW path; per Pi-side spec, mic 0° aligns to servo 90°). There's no compass↔mic-array constant — the acoustic-bearing path uses an operator-calibrated `tripodWorldBearingDeg` (set via MENU → Calibrate Bearing) instead of a static constant, because the compass is on the moving head while the mics are on the fixed tripod, so their relationship isn't a mechanical constant.
- **The solver's barrel-angle bisection is anchored to the LOOK ANGLE, not to horizontal** — `findBarrelAngle` searches `[lookAngle − 2°, lookAngle + 28°]`, and `integrateTrajectory`'s "bullet is gone" floor (`abortBelowM`) is `targetVertical − 50 m`, relative to the target rather than to the muzzle. Both matter: a fixed `[−2°, +28°]` bracket cannot express a downhill shot, so the search saturates on its own lower bound and returns a **plausible-looking but wrong hold-over without reporting failure** (a 300 m / 10° downhill shot gave 7.9° instead of <0.5°). Don't reintroduce a fixed bracket or an absolute floor. `FiringSolutionSolverTest`'s downhill tests are the regression cover.
- **The G1 drag table is verified against an outside reference** — 79 points, Mach 0–5, standard G1 (Ingalls). The previous table paired correct Cd values with the *wrong Mach axis* (subsonic Cd mapped onto the supersonic range), under-applying drag by up to 34% at 1 km. If you touch `G1_TABLE_MACH`/`G1_TABLE_CD`, re-check against an independent ballistic calculator — and note that the test suite's velocity assertions are pinned to reference values, not to the solver's own output, so a test that starts failing after a table edit is evidence about the table.
- **The acoustic SLEW is HORIZONTAL-ONLY — never command the tilt.** A 4-mic TDOA array resolves azimuth only; it has no elevation information, so a slew must leave the camera's tilt exactly where the operator put it. But the Pi's `set_servo_angles` carries both axes and has no "don't move this one" sentinel, so `SniperRepository.slewToAcousticContact` expresses "hold" by reading the current `vertical_deg` (passed in from `SniperViewModel.acceptAudioAlert` via `latchedSensorData`) and sending it straight back. It used to send `RigGeometry.SERVO_VERTICAL_LEVEL_DEG` (90° = horizon) unconditionally, which **recentred the camera on every single slew and threw away the operator's elevation**. "No elevation info" means DON'T COMMAND elevation — not command it to level. The `?.takeIf { it in 0.0..180.0 }` guard is plain validation of the servo's mechanical travel: since `ServoFrame.verticalDeg` is nullable, null already means "no reading" and `0.0` means a genuine full-down aim, so both are handled correctly without heuristics.
- **`ServoFrame.horizontalDeg`/`verticalDeg` are `Float?`** — like `CompassFrame.headingDeg`, and for the same reason: `0°` is a real servo position (full down), not a sentinel, so the previous non-null `0f` default made "field absent from the JSON" indistinguishable from "camera pointing at the ground". `servoHorizontalDeg()`/`servoVerticalDeg()` now return null when the `servo` object **or** the individual field is missing. **Never substitute `0` for a null angle** — that's an aim command, not a default. Diagnostics prints the literal `null` (same convention as the compass heading) so the operator sees the Pi's actual emission.
- **Hardcoded device list** — `availableDevices` in `SniperViewModel` is a fixed 4 entries. Real device discovery isn't implemented.
- **Strings are mostly inlined** — `res/values/strings.xml` only has `app_name`. Most UI strings (chip labels, button text, etc.) are hardcoded literals in Composables. Not translation-ready.
- **`compass.heading_deg` can be JSON `null`** — the Pi's C `build_json` emits the literal token `null` (not a number) when the magnetometer hasn't fixed yet. `CompassFrame.headingDeg` is therefore `Float?`. Always read it via `compassHeadingDeg()` which gates on both `valid` and non-null; never treat a missing heading as `0°` (true north).

## Maintenance — keep this doc current

When you (Claude or human) make changes, update the relevant section here in the same commit. Specifically:

- **Add a new screen?** Update "Repo layout" + "Navigation" + the `AppScreen` enum reference.
- **Add a new WS message type?** Update "Incoming WS message types" with the exact JSON shape.
- **Change the detection flow?** Update the "Detection pipeline — the Pi owns tracking now" section.
- **Add a new dependency?** Update "Tech stack" and note whether it's via the catalog or hardcoded.
- **Add a new permission?** Update "Permissions (manifest)".
- **Discover a new gotcha?** Add it to "Known gotchas". Remove gotchas as they get fixed.
- **Refactor architecture (e.g. add Hilt, switch to Nav Compose, add DataStore)?** Update "Architecture & data flow" and bump the relevant subsections.

The goal: any new Claude session that reads this file should be able to make a sensible code change in 5 minutes without re-exploring the codebase. If you find yourself re-exploring something you "should have known", that's a signal this doc is missing it — add it.
