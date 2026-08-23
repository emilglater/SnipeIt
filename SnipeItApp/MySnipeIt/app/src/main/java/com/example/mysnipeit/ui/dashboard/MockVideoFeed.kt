package com.example.mysnipeit.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.mysnipeit.data.models.DetectedTarget
import com.example.mysnipeit.data.models.TargetType
import kotlinx.coroutines.delay
import kotlin.random.Random

/**
 * Enhanced Mock Video Feed with Target Locking
 * Shows simulated camera view with movable targets and lock/unlock functionality
 */
@Composable
fun MockVideoFeed(
    detectedTargets: List<DetectedTarget>,
    modifier: Modifier = Modifier
) {
    // Track locked targets
    var lockedTargets by remember { mutableStateOf(setOf<String>()) }

    // Generate moving targets with stable positions for locked ones
    val movingTargets = remember { mutableStateListOf<MovingTarget>() }

    // Initialize or update targets
    LaunchedEffect(detectedTargets.size) {
        while (true) {
            // Update only unlocked targets
            movingTargets.forEachIndexed { index, target ->
                if (!lockedTargets.contains(target.id)) {
                    // Move unlocked targets
                    movingTargets[index] = target.copy(
                        x = (target.x + (Random.nextFloat() - 0.5f) * 0.02f).coerceIn(0.1f, 0.9f),
                        y = (target.y + (Random.nextFloat() - 0.5f) * 0.02f).coerceIn(0.1f, 0.9f)
                    )
                }
            }
            delay(100) // Update every 100ms
        }
    }

    // Sync with detected targets
    LaunchedEffect(detectedTargets) {
        if (movingTargets.size != detectedTargets.size) {
            movingTargets.clear()
            detectedTargets.forEachIndexed { index, target ->
                movingTargets.add(
                    MovingTarget(
                        id = target.id,
                        x = target.bbox.x.toFloat() / 1920f, // Normalize to 0-1 range (assuming 1920px width)
                        y = target.bbox.y.toFloat() / 1080f, // Normalize to 0-1 range (assuming 1080px height)
                        type = try {
                            TargetType.valueOf(target.targetType)
                        } catch (e: IllegalArgumentException) {
                            TargetType.UNKNOWN
                        },
                        confidence = target.confidence
                    )
                )
            }
        }
    }

    Box(
        modifier = modifier
            .fillMaxWidth()
            .aspectRatio(16f / 9f)
            .background(Color(0xFF1A1A1A)) // Dark background for video feed
    ) {
        // Simulated camera feed with targets
        Canvas(
            modifier = Modifier.fillMaxSize()
        ) {
            movingTargets.forEach { target ->
                val isLocked = lockedTargets.contains(target.id)

                // Calculate rectangle position and size
                val rectWidth = size.width * 0.15f
                val rectHeight = size.height * 0.25f
                val rectX = (target.x * size.width) - (rectWidth / 2)
                val rectY = (target.y * size.height) - (rectHeight / 2)

                // Choose bright color based on type and lock status
                val color = when {
                    isLocked -> Color(0xFFFF6B35) // Bright Orange for locked
                    target.type == TargetType.HUMAN -> Color(0xFF038C16) // Green
                    else -> Color(0xFF086D80) // Bright Cyan for unknown
                }

                // Draw bounding box with brighter colors
                drawRect(
                    color = color,
                    topLeft = Offset(rectX, rectY),
                    size = Size(rectWidth, rectHeight),
                    style = Stroke(width = if (isLocked) 5f else 3f) // Thicker border when locked
                )

                // Draw corner markers (more visible)
                val cornerSize = 20f
                val corners = listOf(
                    // Top-left
                    Pair(Offset(rectX, rectY), Offset(rectX + cornerSize, rectY)),
                    Pair(Offset(rectX, rectY), Offset(rectX, rectY + cornerSize)),
                    // Top-right
                    Pair(Offset(rectX + rectWidth, rectY), Offset(rectX + rectWidth - cornerSize, rectY)),
                    Pair(Offset(rectX + rectWidth, rectY), Offset(rectX + rectWidth, rectY + cornerSize)),
                    // Bottom-left
                    Pair(Offset(rectX, rectY + rectHeight), Offset(rectX + cornerSize, rectY + rectHeight)),
                    Pair(Offset(rectX, rectY + rectHeight), Offset(rectX, rectY + rectHeight - cornerSize)),
                    // Bottom-right
                    Pair(Offset(rectX + rectWidth, rectY + rectHeight), Offset(rectX + rectWidth - cornerSize, rectY + rectHeight)),
                    Pair(Offset(rectX + rectWidth, rectY + rectHeight), Offset(rectX + rectWidth, rectY + rectHeight - cornerSize))
                )

                corners.forEach { (start, end) ->
                    drawLine(
                        color = color,
                        start = start,
                        end = end,
                        strokeWidth = if (isLocked) 5f else 3f
                    )
                }

                // Draw center crosshair (brighter)
                val centerX = rectX + rectWidth / 2
                val centerY = rectY + rectHeight / 2
                val crosshairSize = 15f

                drawLine(
                    color = color,
                    start = Offset(centerX - crosshairSize, centerY),
                    end = Offset(centerX + crosshairSize, centerY),
                    strokeWidth = 2f
                )
                drawLine(
                    color = color,
                    start = Offset(centerX, centerY - crosshairSize),
                    end = Offset(centerX, centerY + crosshairSize),
                    strokeWidth = 2f
                )

                // Lock indicator (if locked)
                if (isLocked) {
                    // Draw lock icon (simple padlock shape)
                    val lockSize = 15f
                    val lockX = rectX + rectWidth - lockSize - 10f
                    val lockY = rectY + 10f

                    // Lock body
                    drawRect(
                        color = color,
                        topLeft = Offset(lockX, lockY + lockSize * 0.4f),
                        size = Size(lockSize, lockSize * 0.6f),
                        style = Stroke(width = 2f)
                    )

                    // Lock shackle
                    drawArc(
                        color = color,
                        startAngle = 180f,
                        sweepAngle = 180f,
                        useCenter = false,
                        topLeft = Offset(lockX + lockSize * 0.2f, lockY),
                        size = Size(lockSize * 0.6f, lockSize * 0.6f),
                        style = Stroke(width = 2f)
                    )
                }
            }
        }

        // Target info overlays with Lock/Unlock buttons
        movingTargets.forEachIndexed { index, target ->
            val isLocked = lockedTargets.contains(target.id)

            Box(
                modifier = Modifier
                    .fillMaxSize()
            ) {
                Column(
                    modifier = Modifier
                        .align(Alignment.TopStart)
                        .offset(
                            x = (target.x * 400).dp, // Approximate conversion
                            y = (target.y * 225).dp - 60.dp // Position above target
                        )
                        .background(
                            color = Color.Black.copy(alpha = 0.7f),
                            shape = androidx.compose.foundation.shape.RoundedCornerShape(4.dp)
                        )
                        .padding(6.dp)
                ) {
                    // Target info
                    Text(
                        text = "${target.id} | ${target.type.name}",
                        color = when {
                            isLocked -> Color(0xFFFF6B35)
                            target.type == TargetType.HUMAN -> Color(0xFF038C16)
                            else -> Color(0xFF086D80)
                        },
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace
                    )

                    Text(
                        text = "CONF: ${(target.confidence * 100).toInt()}%",
                        color = Color.White,
                        fontSize = 9.sp,
                        fontFamily = FontFamily.Monospace
                    )

                    Spacer(modifier = Modifier.height(4.dp))

                    // Lock/Unlock button
                    Button(
                        onClick = {
                            if (isLocked) {
                                // Unlock
                                lockedTargets = lockedTargets - target.id
                            } else {
                                // Lock
                                lockedTargets = lockedTargets + target.id
                            }
                        },
                        modifier = Modifier
                            .height(24.dp)
                            .fillMaxWidth(),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (isLocked) {
                                Color(0xFFFF6B35) // Orange when locked
                            } else {
                                Color(0xFF038C16) // Green when unlocked
                            }
                        ),
                        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 2.dp)
                    ) {
                        Text(
                            text = if (isLocked) "UNLOCK" else "LOCK",
                            fontSize = 9.sp,
                            fontWeight = FontWeight.Bold,
                            color = Color.Black,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                }
            }
        }

        // Scanning mode indicator (top-left)
        Box(
            modifier = Modifier
                .align(Alignment.TopStart)
                .padding(16.dp)
                .background(
                    color = Color.Black.copy(alpha = 0.7f),
                    shape = androidx.compose.foundation.shape.RoundedCornerShape(4.dp)
                )
                .padding(horizontal = 12.dp, vertical = 6.dp)
        ) {
            val hasLockedTargets = lockedTargets.isNotEmpty()
            Text(
                text = if (hasLockedTargets) {
                    " TRACKING (${lockedTargets.size} locked)"
                } else {
                    " SCANNING"
                },
                color = if (hasLockedTargets) Color(0xFFFF6B35) else Color(0xFF038C16),
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace
            )
        }

        // Crosshair in center (always visible)
        Canvas(
            modifier = Modifier
                .align(Alignment.Center)
                .size(40.dp)
        ) {
            val center = Offset(size.width / 2, size.height / 2)
            val crosshairLength = size.width / 2

            // Horizontal line
            drawLine(
                color = Color(0xFF038C16).copy(alpha = 0.5f),
                start = Offset(0f, center.y),
                end = Offset(size.width, center.y),
                strokeWidth = 2f
            )

            // Vertical line
            drawLine(
                color = Color(0xFF038C16).copy(alpha = 0.5f),
                start = Offset(center.x, 0f),
                end = Offset(center.x, size.height),
                strokeWidth = 2f
            )

            // Center dot
            drawCircle(
                color = Color(0xFF038C16),
                radius = 3f,
                center = center
            )
        }
    }
}

/**
 * Data class for moving targets in the mock video
 */
private data class MovingTarget(
    val id: String,
    var x: Float, // 0.0 to 1.0 (relative position)
    var y: Float, // 0.0 to 1.0 (relative position)
    val type: TargetType,
    val confidence: Float
)