package com.example.mysnipeit.ui.dashboard

import android.content.Context
import android.util.Log
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.exoplayer.DefaultLoadControl
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.rtsp.RtspMediaSource
import androidx.media3.exoplayer.source.MediaSource
import androidx.media3.ui.PlayerView
import com.example.mysnipeit.R
import com.example.mysnipeit.data.ballistics.FiringSolution
import com.example.mysnipeit.data.models.DetectedTarget
import com.example.mysnipeit.data.network.WifiPerfLock
import com.example.mysnipeit.ui.theme.*
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import com.example.mysnipeit.data.models.ConnectionState

// Stall watchdog tuning. The RTSP session can wedge after a Wi-Fi blackout —
// frames stop rendering even though the transport recovers — and ExoPlayer has
// no native recovery for it. We poll playback progress and, if it hasn't
// advanced for STALL_TIMEOUT_MS while we're supposed to be playing, rebuild the
// RTSP session (what a manual back-out-and-reconnect does). A fresh session also
// gets a keyframe within ~1 s, which resets any accumulated latency.
private const val STALL_POLL_INTERVAL_MS = 500L
private const val STALL_TIMEOUT_MS = 3_000L
private const val STALL_RECONNECT_INITIAL_BACKOFF_MS = 1_000L
private const val STALL_RECONNECT_MAX_BACKOFF_MS = 8_000L

/**
 * Build the RTSP media source. When [forceTcp] is true, forces RTP-over-TCP
 * (matches the Pi's `-rtsp_transport tcp`, reliable but stalls on loss); when
 * false, leaves the library default (UDP with TCP fallback), which degrades
 * more gracefully on a lossy link. Extracted so the initial load and the stall
 * watchdog build it identically.
 */
private fun buildRtspMediaSource(url: String, forceTcp: Boolean): MediaSource =
    RtspMediaSource.Factory()
        .apply { if (forceTcp) setForceUseRtpTcp(true) }
        .setTimeoutMs(8000)
        .createMediaSource(MediaItem.fromUri(url))


// Video resolution constants (hardcoded for POC)
private const val VIDEO_WIDTH = 1920f
private const val VIDEO_HEIGHT = 1080f

@Composable
fun TacticalVideoPlayer(
    detectedTargets: List<DetectedTarget>,
    firingSolution: FiringSolution?,
    selectedTargetId: String?,
    connectionState: ConnectionState,
    streamReady: Boolean,
    videoStreamUrl: String?,
    onTargetClick: (DetectedTarget) -> Unit = {},
    onTargetLockToggle: (String, Boolean) -> Unit = { _, _ -> },
    onTargetSelect: (String) -> Unit = {},
    // Reports actual video health: true while frames are rendering, false when
    // stalled / reconnecting / not streaming. Drives the dashboard's VIDEO chip
    // from real frame flow instead of the WS control-channel state.
    onVideoHealthChanged: (Boolean) -> Unit = {},
    // RTSP transport: true = force TCP (default), false = UDP w/ TCP fallback.
    // Debug toggle for A/B testing on the lossy AP link. Flipping it reloads
    // the stream with the new transport.
    forceTcp: Boolean = true,
    // Heights of any HUD bars the parent draws OVER this player's edges (the
    // dashboard's bottom sensor strip; the top is 0 today since the TopBar
    // stacks above the video region instead of overlaying it). Used to keep
    // each bbox info card — and its LOCK button — out of the covered zones.
    topOverlayObstruction: Dp = 0.dp,
    bottomOverlayObstruction: Dp = 0.dp,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val exoPlayer = remember { createExoPlayer(context) }

    var scanlinePosition by remember { mutableStateOf(0f) }
    var videoTime by remember { mutableStateOf(0L) }

    // NOTE: lock state is NOT held locally. `selectedTargetId` (from the
    // ViewModel) is the single source of truth for "which target is locked" —
    // the marker, the firing card, and the bottom UNLOCK button all read it, so
    // they can't desync. A locked target that leaves the frame keeps
    // selectedTargetId set (so UNLOCK still works); its marker/card just aren't
    // drawn until it returns.

    // Load the stream whenever it's ready — independent of the WS control
    // channel. The RTSP video is a separate connection to the same host, so a
    // transient WS blip must NOT tear it down (the WS close no longer clears
    // rtspStreamUrl either; see RaspberryPiClient.onClose). Playback is torn
    // down only when the stream is genuinely gone (operator left → disconnect()
    // nulls the url).
    LaunchedEffect(streamReady, videoStreamUrl, forceTcp) {
        if (streamReady && videoStreamUrl != null) {
            Log.d("TacticalVideoPlayer", "Stream ready signal received, loading: $videoStreamUrl (tcp=$forceTcp)")
            exoPlayer.setMediaSource(buildRtspMediaSource(videoStreamUrl, forceTcp))
            exoPlayer.playWhenReady = true
            exoPlayer.prepare()
        } else {
            if (!streamReady) {
                Log.d("TacticalVideoPlayer", "Waiting for stream_ready signal from server...")
            }
            exoPlayer.stop()
            exoPlayer.clearMediaItems()
        }
    }

    // Stall watchdog + auto-reconnect. Detects a wedged/frozen RTSP session
    // (playback position not advancing while we're supposed to be playing) and
    // rebuilds it — self-healing what previously required the operator to back
    // out and reconnect by hand. Retries with exponential backoff while a stream
    // is expected; resets on healthy playback. Gated on `streamReady` only, NOT
    // the WS control channel — video is independent of a WS blip. Also emits the
    // frame-flow health used by the dashboard VIDEO chip. Reads latest url /
    // streamReady / callback via rememberUpdatedState so the loop never restarts.
    val currentUrl by rememberUpdatedState(videoStreamUrl)
    val streamExpected by rememberUpdatedState(streamReady)
    val healthCb by rememberUpdatedState(onVideoHealthChanged)
    val currentForceTcp by rememberUpdatedState(forceTcp)
    LaunchedEffect(Unit) {
        var lastSeenUrl: String? = null
        var lastPos = 0L
        var lastProgressAt = System.currentTimeMillis()
        var backoffMs = STALL_RECONNECT_INITIAL_BACKOFF_MS
        var reportedHealthy: Boolean? = null
        // Only fires the callback when health actually flips, so the UI isn't
        // spammed every poll.
        fun reportHealth(healthy: Boolean) {
            if (reportedHealthy != healthy) {
                reportedHealthy = healthy
                healthCb(healthy)
            }
        }
        while (isActive) {
            delay(STALL_POLL_INTERVAL_MS)
            val url = currentUrl
            // Not streaming (or a new stream just loaded): keep timers fresh so
            // we never trigger a spurious reconnect during setup / teardown.
            if (url == null || !streamExpected || url != lastSeenUrl) {
                lastSeenUrl = url
                lastPos = exoPlayer.currentPosition
                lastProgressAt = System.currentTimeMillis()
                backoffMs = STALL_RECONNECT_INITIAL_BACKOFF_MS
                if (url == null || !streamExpected) reportHealth(false)
                continue
            }
            val pos = exoPlayer.currentPosition
            val now = System.currentTimeMillis()
            if (pos != lastPos) {
                // Progress → healthy. Reset the stall timer and the backoff.
                lastPos = pos
                lastProgressAt = now
                backoffMs = STALL_RECONNECT_INITIAL_BACKOFF_MS
                reportHealth(true)
                continue
            }
            // Position frozen. Reconnect once it's been stuck past the timeout.
            if (exoPlayer.playWhenReady && (now - lastProgressAt) >= STALL_TIMEOUT_MS) {
                Log.w(
                    "TacticalVideoPlayer",
                    "Stall detected (${now - lastProgressAt}ms no progress) — rebuilding RTSP session"
                )
                reportHealth(false)
                exoPlayer.stop()
                exoPlayer.clearMediaItems()
                exoPlayer.setMediaSource(buildRtspMediaSource(url, currentForceTcp))
                exoPlayer.playWhenReady = true
                exoPlayer.prepare()
                // Give the new session time to come up before re-evaluating,
                // backing off so a truly-down server isn't hammered.
                lastPos = 0L
                lastProgressAt = System.currentTimeMillis()
                delay(backoffMs)
                backoffMs = (backoffMs * 2).coerceAtMost(STALL_RECONNECT_MAX_BACKOFF_MS)
            }
        }
    }

    // Hold a high-performance Wi-Fi lock (+ CPU wake lock) while the live view
    // is open, to suppress client-side Wi-Fi power-save — the interaction that
    // produced the periodic radio blackouts freezing the stream. See WifiPerfLock.
    DisposableEffect(Unit) {
        WifiPerfLock.acquire(context)
        onDispose { WifiPerfLock.release() }
    }

    // Animate scanning line
    LaunchedEffect(Unit) {
        while (true) {
            scanlinePosition = 0f
            while (scanlinePosition < 1f) {
                scanlinePosition += 0.002f
                delay(16) // ~60 FPS
            }
            delay(1000)
        }
    }

    // Track video playback time
    LaunchedEffect(Unit) {
        while (true) {
            videoTime = exoPlayer.currentPosition
            delay(100)
        }
    }

    DisposableEffect(Unit) {
        onDispose {
            exoPlayer.release()
        }
    }

    BoxWithConstraints(
        modifier = modifier
            .fillMaxSize()
            .background(Color.Black),
        contentAlignment = Alignment.Center
    ) {
        // How far the parent's HUD bars reach INTO the centered 16:9 video
        // box. The video is centered in this region, so letterbox slack above/
        // below it absorbs part of each obstruction before the video itself is
        // covered. Whatever remains is the zone info cards must avoid (the
        // bars are drawn over the video, z-above us).
        val videoBoxHeight = minOf(maxWidth * (VIDEO_HEIGHT / VIDEO_WIDTH), maxHeight)
        val letterbox = (maxHeight - videoBoxHeight) / 2
        val topOverlayIntrusion = (topOverlayObstruction - letterbox).coerceAtLeast(0.dp)
        val bottomOverlayIntrusion = (bottomOverlayObstruction - letterbox).coerceAtLeast(0.dp)

        // Constrain the video + overlay region to the source 16:9 aspect ratio.
        // This keeps detection bbox coordinates aligned with the rendered video
        // even on tablets that aren't exactly 16:9 (no pillarbox/letterbox math
        // needed in EnhancedTargetMarker).
        Box(
            modifier = Modifier
                .aspectRatio(VIDEO_WIDTH / VIDEO_HEIGHT)
                .background(Color.Black)
        ) {
            // Video player
            AndroidView(
                factory = { ctx ->
                    PlayerView(ctx).apply {
                        player = exoPlayer
                        useController = false
                        setShowBuffering(PlayerView.SHOW_BUFFERING_NEVER)
                    }
                },
                modifier = Modifier.fillMaxSize()
            )

            // Tactical overlays
            Box(modifier = Modifier.fillMaxSize()) {
            // Crosshair
           // CrosshairOverlay()

            // Scanning line
           // ScanLineOverlay(scanlinePosition)

            // Target markers
            //SimulatedTargets(videoTime).forEach { target ->
            detectedTargets.forEach { target ->
                // key(target.id) ensures Compose preserves the same composable
                // instance (and its animation state) for the same target across
                // recompositions. Without it, position-based identity would
                // shuffle when the target list reorders, breaking interpolation.
                key(target.id) {
                    // Locked iff this is the selected target (single source of
                    // truth — see the note where lockedTargetId used to live).
                    val isLocked = target.id == selectedTargetId
                    // Only CONFIRMED tracks are lockable — the Pi rejects a lock
                    // on an unconfirmed/fallback track (would fall back to the
                    // highest-confidence one). confirmed == null means the whole
                    // message was fallback/overlay-only.
                    val lockable = target.confirmed == true

                    EnhancedTargetMarker(
                        target = target,
                        isLocked = isLocked,
                        isSelected = isLocked,
                        lockable = lockable,
                        topOverlayIntrusion = topOverlayIntrusion,
                        bottomOverlayIntrusion = bottomOverlayIntrusion,
                        onLockClick = {
                            if (isLocked) {
                                // Unlock current target (always allowed).
                                onTargetSelect("")  // clears selectedTargetId
                                onTargetLockToggle(target.id, false)  // unlock command
                            } else if (lockable) {
                                // Switching from another locked target: release
                                // it first so the Pi isn't left following it.
                                selectedTargetId?.let { prev ->
                                    if (prev != target.id) onTargetLockToggle(prev, false)
                                }
                                onTargetSelect(target.id)
                                onTargetLockToggle(target.id, true)  // lock command
                            }
                            // else: unconfirmed → ignore the lock attempt.
                        },
                        onTargetClick = {}
                    )
                }
            }

            // Video status
            VideoStatusOverlay(
                modifier = Modifier.align(Alignment.TopStart)
            )

            // Scanning/Tracking mode indicator
            Box(
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(16.dp)
                    .background(
                        color = Color.Black.copy(alpha = 0.7f),
                        shape = RoundedCornerShape(4.dp)
                    )
                    .padding(horizontal = 12.dp, vertical = 6.dp)
            ) {
                val hasLockedTarget = !selectedTargetId.isNullOrEmpty()

                Text(
                    text = if (hasLockedTarget) "TARGET LOCKED" else "SCANNING",
                    color = if (hasLockedTarget) Color(0xFFFFAA00) else Color(0xFF038C16),
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace
                )
            }
            } // end overlays Box
        } // end aspect-ratio video Box
    } // end outer black Box
}

private fun createExoPlayer(context: Context): ExoPlayer {
    // Low-latency LoadControl tuned for live RTSP. ExoPlayer's defaults are
    // VOD-oriented (~50s buffer) which adds 1-3s of perceived lag. These
    // values keep us under ~1s of buffered data.
    val loadControl = DefaultLoadControl.Builder()
        .setBufferDurationsMs(
            /* minBufferMs = */              250,
            /* maxBufferMs = */              1000,
            /* bufferForPlaybackMs = */      100,
            /* bufferForPlaybackAfterRebufferMs = */ 250
        )
        .setPrioritizeTimeOverSizeThresholds(true)
        .build()

    return ExoPlayer.Builder(context)
        .setLoadControl(loadControl)
        .build().apply {
            repeatMode = Player.REPEAT_MODE_ALL

            addListener(object : Player.Listener {
                override fun onPlayerError(error: PlaybackException) {
                    android.util.Log.e("ExoPlayer", "Playback error: ${error.message}", error)
                }
            })
        }
}

@Composable
private fun SimulatedTargets(videoTime: Long): List<DetectedTarget> {
    val targets = mutableListOf<DetectedTarget>()

    if (videoTime > 2000) {
        targets.add(
            DetectedTarget(
                id = "T1",
                targetType = "HUMAN",
                confidence = 0.85f,
                bbox = com.example.mysnipeit.data.models.BoundingBox(
                    x = 576,      // ~30% of 1920
                    y = 432,      // ~40% of 1080
                    width = 150,
                    height = 250
                )
            )
        )
    }

    if (videoTime > 8000) {
        targets.add(
            DetectedTarget(
                id = "T2",
                targetType = "UNKNOWN",
                confidence = 0.72f,
                bbox = com.example.mysnipeit.data.models.BoundingBox(
                    x = 1344,     // ~70% of 1920
                    y = 540,      // ~50% of 1080
                    width = 200,
                    height = 180
                )
            )
        )
    }

    if (videoTime > 15000 && videoTime < 25000) {
        targets.add(
            DetectedTarget(
                id = "T3",
                targetType = "HUMAN",
                confidence = 0.91f,
                bbox = com.example.mysnipeit.data.models.BoundingBox(
                    x = 960,      // ~50% of 1920 (center)
                    y = 486,      // ~45% of 1080
                    width = 120,
                    height = 220
                )
            )
        )
    }

    return targets
}

// Minimum touch-target size for a bbox. A person 150m+ out can shrink to a
// ~30px box — well below a comfortable fingertip. The gesture area expands
// symmetrically around the drawn bbox up to at least this square.
private val MIN_TAP_TARGET = 48.dp

@Composable
private fun EnhancedTargetMarker(
    target: DetectedTarget,
    isLocked: Boolean,
    isSelected: Boolean,
    lockable: Boolean,
    // How far the dashboard's HUD bars cover the top/bottom of the video box
    // (see TacticalVideoPlayer). The info card is kept out of both zones so
    // its LOCK button always stays pressable.
    topOverlayIntrusion: Dp,
    bottomOverlayIntrusion: Dp,
    onLockClick: () -> Unit,
    onTargetClick: () -> Unit
) {
    // Unconfirmed / fallback tracks are ghosted and not lockable (the Pi only
    // accepts a lock on confirmed tracks). A locked track always renders full.
    val dimAlpha = if (lockable || isLocked) 1f else 0.4f
    // Tactical palette — see ui/theme/Color.kt.
    // Bone for tracked targets, copper for the selected/locked target.
    // No saturated greens or cyans (the redesign brief).
    val palette = LocalTactical.current
    // High-visibility bbox colors over the live camera feed:
    //   regular tracking → bright orange
    //   locked target    → vivid red (high-alert)
    // Values are identical in light + dark modes because the video region is
    // always dark (real camera feed); contrast is against the video, not the
    // surrounding UI.
    val markerColor = when {
        isLocked -> palette.bboxLocked
        else     -> palette.bboxTracked
    }

    var pulseAlpha by remember { mutableStateOf(1f) }

    LaunchedEffect(target.id, isLocked, isSelected) {
        if (!isLocked && !isSelected) {
            while (true) {
                pulseAlpha = 0.4f
                delay(600)
                pulseAlpha = 1f
                delay(600)
            }
        } else {
            pulseAlpha = 1f
        }
    }

    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
        // Convert bbox pixel coordinates (relative to 1920x1080) to local Dp.
        // The parent BoxWithConstraints is the 16:9 video region (set up in the
        // outer aspect-ratio Box), so these ratios map 1:1 to rendered video pixels.
        val targetX = maxWidth  * (target.bbox.x.toFloat()      / VIDEO_WIDTH)
        val targetY = maxHeight * (target.bbox.y.toFloat()      / VIDEO_HEIGHT)
        val targetW = maxWidth  * (target.bbox.width.toFloat()  / VIDEO_WIDTH)
        val targetH = maxHeight * (target.bbox.height.toFloat() / VIDEO_HEIGHT)

        // No tween — snap the bbox straight to the Pi's reported position so
        // the outdoor run sees the Pi tracker's RAW box motion with zero app
        // interference. (Pure-visual tweening is permitted and can be added back
        // after the outdoor run; see the detection-contract handoff.)
        val xPos      = targetX
        val yPos      = targetY
        val boxWidth  = targetW
        val boxHeight = targetH

        // Gesture area: the bbox itself, expanded to at least MIN_TAP_TARGET
        // per side so distant (tiny) targets stay pressable.
        //   double-tap → lock / unlock: same action as the info card's LOCK
        //                button (onLockClick already guards on `lockable`).
        //                Exists because the card/button can be covered by a
        //                HUD bar or be fiddly to hit on a moving target.
        //   single-tap → onTargetClick (currently a no-op at the call site,
        //                kept for parity with the previous clickable).
        // rememberUpdatedState keeps the handlers fresh inside pointerInput,
        // which is keyed on Unit and would otherwise capture stale closures.
        val hitW = maxOf(boxWidth, MIN_TAP_TARGET)
        val hitH = maxOf(boxHeight, MIN_TAP_TARGET)
        val currentOnLockClick by rememberUpdatedState(onLockClick)
        val currentOnTargetClick by rememberUpdatedState(onTargetClick)

        Box(
            modifier = Modifier
                .offset(
                    x = xPos - (hitW - boxWidth) / 2,
                    y = yPos - (hitH - boxHeight) / 2,
                )
                .size(width = hitW, height = hitH)
                .alpha(dimAlpha)
                .pointerInput(Unit) {
                    detectTapGestures(
                        onDoubleTap = { currentOnLockClick() },
                        onTap = { currentOnTargetClick() },
                    )
                },
            contentAlignment = Alignment.Center
        ) {
            // Target rectangle drawn at the ACTUAL bbox size (the parent hit
            // Box may be larger for small targets — drawing stays truthful).
            Canvas(modifier = Modifier.size(width = boxWidth, height = boxHeight)) {
                val w = size.width
                val h = size.height
                val strokeWidth = when {
                    isSelected -> 5f
                    isLocked   -> 4f
                    else       -> 3f
                }

                // Main bounding rectangle
                drawRect(
                    color = markerColor.copy(alpha = pulseAlpha),
                    topLeft = Offset(0f, 0f),
                    size = Size(w, h),
                    style = Stroke(width = strokeWidth)
                )

                // Pulsing outer glow for selected target
                if (isSelected) {
                    drawRect(
                        color = markerColor.copy(alpha = 0.3f),
                        topLeft = Offset(-4f, -4f),
                        size = Size(w + 8, h + 8),
                        style = Stroke(width = 2f)
                    )
                }

                // Corner brackets, scaled to ~20% of the smaller bbox side
                val bracketSize = (minOf(w, h) * 0.2f).coerceAtMost(24f)
                val corners = listOf(
                    Offset(0f, 0f),                       // top-left
                    Offset(w - bracketSize, 0f),          // top-right
                    Offset(0f, h - bracketSize),          // bottom-left
                    Offset(w - bracketSize, h - bracketSize) // bottom-right
                )
                corners.forEach { pos ->
                    drawLine(markerColor, pos, Offset(pos.x + bracketSize, pos.y), strokeWidth)
                    drawLine(markerColor, pos, Offset(pos.x, pos.y + bracketSize), strokeWidth)
                }

                // Center crosshair, scaled to ~10% of the smaller side
                val cx = w / 2f
                val cy = h / 2f
                val crosshair = (minOf(w, h) * 0.1f).coerceAtMost(14f)
                drawLine(markerColor, Offset(cx - crosshair, cy), Offset(cx + crosshair, cy), 2f)
                drawLine(markerColor, Offset(cx, cy - crosshair), Offset(cx, cy + crosshair), 2f)

                // Lock indicator (top-right inside the bbox)
                if (isLocked) {
                    val lockSize = 12f
                    val lockX = w - lockSize - 8f
                    val lockY = 8f
                    drawRect(
                        color = markerColor,
                        topLeft = Offset(lockX, lockY + lockSize * 0.4f),
                        size = Size(lockSize, lockSize * 0.6f),
                        style = Stroke(width = 2f)
                    )
                    drawArc(
                        color = markerColor,
                        startAngle = 180f,
                        sweepAngle = 180f,
                        useCenter = false,
                        topLeft = Offset(lockX + lockSize * 0.2f, lockY),
                        size = Size(lockSize * 0.6f, lockSize * 0.6f),
                        style = Stroke(width = 2f)
                    )
                }

                // Selection indicator (asterisk top-left inside the bbox)
                if (isSelected) {
                    val starSize = 8f
                    val starX = 8f + starSize
                    val starY = 8f + starSize
                    drawLine(markerColor, Offset(starX - starSize, starY),
                             Offset(starX + starSize, starY), 3f)
                    drawLine(markerColor, Offset(starX, starY - starSize),
                             Offset(starX, starY + starSize), 3f)
                }
            }

        }

        // Minimal info card: "T1 | HUMAN" + LOCK button (or UNCONFIRMED).
        // Placement keeps the card — and its button — fully visible:
        //   - below the bbox by default, flipped ABOVE when "below" would run
        //     into the sensor strip's covered zone (bottomOverlayIntrusion)
        //     or off the bottom of the video;
        //   - when flipped above, clamped below topOverlayIntrusion so it
        //     can't disappear under a top bar either;
        //   - clamped horizontally into the video box, so edge targets don't
        //     squeeze the card into wrapping its text.
        // The card is a SIBLING of the bbox Box (not a child), so its size is
        // unconstrained by narrow bboxes. Its real size is measured via
        // onSizeChanged; it stays invisible for the first frame until then.
        var cardSize by remember { mutableStateOf(IntSize.Zero) }
        val density = LocalDensity.current
        val cardW = with(density) { cardSize.width.toDp() }
        val cardH = with(density) { cardSize.height.toDp() }
        val cardGap = 6.dp
        val usableTop = topOverlayIntrusion
        val usableBottom = maxHeight - bottomOverlayIntrusion
        val belowY = yPos + boxHeight + cardGap
        val cardY =
            if (belowY + cardH > usableBottom) (yPos - cardGap - cardH).coerceAtLeast(usableTop)
            else belowY
        val cardX = (xPos + (boxWidth - cardW) / 2)
            .coerceIn(0.dp, (maxWidth - cardW).coerceAtLeast(0.dp))

        Card(
            modifier = Modifier
                .offset(x = cardX, y = cardY)
                .onSizeChanged { cardSize = it }
                .alpha(if (cardSize == IntSize.Zero) 0f else dimAlpha),
            colors = CardDefaults.cardColors(
                containerColor = Color.Black.copy(alpha = 0.85f)
            ),
            shape = RoundedCornerShape(6.dp)
        ) {
            Column(
                modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {

                Text(
                    text = "${target.id} | ${target.targetType}",
                    color = markerColor,
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold
                )

                Text(
                    text = "CONF: ${(target.confidence * 100).toInt()}%",
                    color = when {
                        target.confidence > 0.8f -> Color(0xFF038C16)
                        target.confidence > 0.6f -> Color(0xFFFFAA00)
                        else                     -> Color(0xFFFF4444)
                    },
                    fontSize = 9.sp,
                    fontFamily = FontFamily.Monospace
                )

                Spacer(modifier = Modifier.height(4.dp))

                // LOCK affordance only on confirmed (lockable) tracks, or to
                // release an already-locked one. Unconfirmed/fallback tracks
                // show WHY they can't be locked instead of a dead button.
                if (lockable || isLocked) {
                    Button(
                        onClick = onLockClick,
                        modifier = Modifier
                            .height(26.dp)
                            .widthIn(min = 70.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (isLocked) Color(0xFFFFAA00)
                                             else Color(0xFF038C16)
                        ),
                        shape = RoundedCornerShape(4.dp),
                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp)
                    ) {
                        Text(
                            text = if (isLocked) "UNLOCK" else "LOCK",
                            fontSize = 10.sp,
                            fontWeight = FontWeight.Bold,
                            color = Color.Black,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                } else {
                    Text(
                        text = "UNCONFIRMED",
                        fontSize = 9.sp,
                        fontWeight = FontWeight.Bold,
                        color = Color(0xFFFFAA00),
                        fontFamily = FontFamily.Monospace
                    )
                }
            }
        }
    }
}

// Keep existing CrosshairOverlay, ScanLineOverlay, and VideoStatusOverlay functions unchanged
@Composable
private fun CrosshairOverlay() {
    Canvas(modifier = Modifier.fillMaxSize()) {
        val centerX = size.width / 2
        val centerY = size.height / 2
        val crosshairSize = 40f
        val color = Color(0xFF00FF41)

        drawLine(
            color = color.copy(alpha = 0.7f),
            start = Offset(centerX - crosshairSize, centerY),
            end = Offset(centerX + crosshairSize, centerY),
            strokeWidth = 2f
        )
        drawLine(
            color = color.copy(alpha = 0.7f),
            start = Offset(centerX, centerY - crosshairSize),
            end = Offset(centerX, centerY + crosshairSize),
            strokeWidth = 2f
        )

        drawCircle(
            color = color,
            radius = 3f,
            center = Offset(centerX, centerY)
        )

        for (i in 1..3) {
            drawCircle(
                color = color.copy(alpha = 0.3f),
                radius = crosshairSize * i,
                center = Offset(centerX, centerY),
                style = Stroke(
                    width = 1f,
                    pathEffect = PathEffect.dashPathEffect(floatArrayOf(5f, 5f))
                )
            )
        }
    }
}

@Composable
private fun ScanLineOverlay(position: Float) {
    Canvas(modifier = Modifier.fillMaxSize()) {
        val y = size.height * position
        val color = Color(0xFF00FF41)

        drawLine(
            color = color.copy(alpha = 0.5f),
            start = Offset(0f, y),
            end = Offset(size.width, y),
            strokeWidth = 2f
        )

        val fadeHeight = 80f
        for (i in 0 until fadeHeight.toInt() step 2) {
            val alpha = (1f - (i / fadeHeight)) * 0.2f
            drawLine(
                color = color.copy(alpha = alpha),
                start = Offset(0f, y + i),
                end = Offset(size.width, y + i),
                strokeWidth = 1f
            )
        }
    }
}

@Composable
private fun VideoStatusOverlay(modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .padding(16.dp)
            .background(Color.Black.copy(alpha = 0.7f), RoundedCornerShape(4.dp))
            .padding(8.dp)
    ) {
        Row(
            horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(8.dp)
                    .background(StatusConnected, CircleShape)
            )
            Text(
                text = "LIVE",
                color = StatusConnected,
                fontSize = 10.sp,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold
            )
        }

        Text(
            text = "1920x1080",
            color = MilitaryTextSecondary,
            fontSize = 8.sp,
            fontFamily = FontFamily.Monospace
        )
    }
}