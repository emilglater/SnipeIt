package com.example.mysnipeit.ui.theme

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp

/**
 * Shared tactical UI primitives — direct port of the helpers from the
 * design's `screens-final.jsx`:
 *
 *   - [Lbl]    : small uppercase mono label
 *   - [Chip]   : bordered tag with 5 tones
 *   - [Stat]   : label + big mono value + optional unit
 *   - [Bracket]: container framed by 4 corner brackets only
 *   - [TopBar] : 36dp tall top bar present on every main screen
 *   - [ThemeToggle] : DARK / LIGHT pill switcher for the TopBar
 *
 * All read [LocalTactical] for colors so they automatically follow the
 * light/dark theme.
 */

// ----------------------------------------------------------------------------
// Lbl
// ----------------------------------------------------------------------------
@Composable
fun Lbl(
    text: String,
    modifier: Modifier = Modifier,
    color: Color? = null,
) {
    val t = LocalTactical.current
    Text(
        text = text.uppercase(),
        color = color ?: t.inkDim,
        fontSize = 10.sp,
        letterSpacing = 0.18.em,
        fontFamily = JetBrainsMono,
        modifier = modifier,
    )
}

// ----------------------------------------------------------------------------
// Chip
// ----------------------------------------------------------------------------
enum class ChipTone { Dim, On, Warn, Danger, Solid }

@Composable
fun Chip(
    text: String,
    tone: ChipTone = ChipTone.Dim,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    val (fg, border, bg) = when (tone) {
        ChipTone.Dim    -> Triple(t.inkDim, t.line,      Color.Transparent)
        ChipTone.On     -> Triple(t.on,     t.lineHi,    Color.Transparent)
        ChipTone.Warn   -> Triple(t.accent, t.accentDim, Color.Transparent)
        ChipTone.Danger -> Triple(t.danger, t.danger,    Color.Transparent)
        ChipTone.Solid  -> Triple(t.ink,    t.lineHi,    t.panelHi)
    }
    Box(
        modifier = modifier
            .background(bg)
            .border(1.dp, border)
            .padding(horizontal = 8.dp, vertical = 3.dp)
    ) {
        Text(
            text = text.uppercase(),
            color = fg,
            fontSize = 10.sp,
            letterSpacing = 0.14.em,
            fontFamily = JetBrainsMono,
            maxLines = 1,
            softWrap = false,
        )
    }
}

// ----------------------------------------------------------------------------
// Stat
// ----------------------------------------------------------------------------
enum class StatSize(val fontSize: TextUnit) {
    Sm(18.sp), Md(22.sp), Lg(32.sp),
}

@Composable
fun Stat(
    label: String,
    value: String,
    unit: String? = null,
    size: StatSize = StatSize.Md,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Column(modifier) {
        Lbl(label)
        Spacer(Modifier.height(4.dp))
        Row(verticalAlignment = Alignment.Bottom) {
            Text(
                text = value,
                color = t.ink,
                fontSize = size.fontSize,
                lineHeight = size.fontSize,
                fontFamily = JetBrainsMono,
            )
            if (unit != null) {
                Spacer(Modifier.width(4.dp))
                Text(
                    text = unit,
                    color = t.inkMute,
                    fontSize = 11.sp,
                    fontFamily = JetBrainsMono,
                )
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Bracket (corner-only border)
// ----------------------------------------------------------------------------
@Composable
fun Bracket(
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(12.dp),
    content: @Composable BoxScope.() -> Unit,
) {
    val t = LocalTactical.current
    Box(
        modifier = modifier.drawBehind {
            val s = 10.dp.toPx()
            val stroke = 1.dp.toPx()
            val w = size.width
            val h = size.height
            // Top-left
            drawLine(t.lineHi, Offset(0f, 0f), Offset(s, 0f), stroke)
            drawLine(t.lineHi, Offset(0f, 0f), Offset(0f, s), stroke)
            // Top-right
            drawLine(t.lineHi, Offset(w - s, 0f), Offset(w, 0f), stroke)
            drawLine(t.lineHi, Offset(w, 0f), Offset(w, s), stroke)
            // Bottom-left
            drawLine(t.lineHi, Offset(0f, h - s), Offset(0f, h), stroke)
            drawLine(t.lineHi, Offset(0f, h), Offset(s, h), stroke)
            // Bottom-right
            drawLine(t.lineHi, Offset(w - s, h), Offset(w, h), stroke)
            drawLine(t.lineHi, Offset(w, h - s), Offset(w, h), stroke)
        }
    ) {
        Box(Modifier.padding(contentPadding), content = content)
    }
}

// ----------------------------------------------------------------------------
// TopBar
// ----------------------------------------------------------------------------
/**
 * 36dp tall header used on every main screen. Left = optional back button +
 * brand + device context, right = caller-supplied content (status chips,
 * theme toggle, clock, etc.).
 *
 * When [onBackClick] is provided, a small "← BACK" button is rendered as the
 * leftmost element. Putting it here means the back affordance lives in the
 * same place on every screen (HomeScreen passes `null` since it's the root).
 */
@Composable
fun TopBar(
    device: String,
    modifier: Modifier = Modifier,
    onBackClick: (() -> Unit)? = null,
    extra: @Composable (RowScope.() -> Unit) = {},
) {
    val t = LocalTactical.current
    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(36.dp)
            .background(t.panel)
            .drawBehind {
                drawLine(
                    color = t.line,
                    start = Offset(0f, size.height),
                    end = Offset(size.width, size.height),
                    strokeWidth = 1.dp.toPx(),
                )
            }
            .padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            if (onBackClick != null) {
                Box(
                    modifier = Modifier
                        .border(1.dp, t.line)
                        .clickable(onClick = onBackClick)
                        .padding(horizontal = 10.dp, vertical = 4.dp),
                ) {
                    Text(
                        text = "← BACK",
                        color = t.ink,
                        fontSize = 9.sp,
                        letterSpacing = 0.18.em,
                        fontFamily = JetBrainsMono,
                    )
                }
            }
            Text(
                text = "SNIPEIT // OPS",
                color = t.ink,
                fontSize = 10.sp,
                letterSpacing = 0.14.em,
                fontFamily = JetBrainsMono,
                fontWeight = FontWeight.Medium,
            )
            Text(
                text = "│",
                color = t.inkMute,
                fontSize = 10.sp,
                fontFamily = JetBrainsMono,
            )
            Text(
                text = device.uppercase(),
                color = t.inkDim,
                fontSize = 10.sp,
                letterSpacing = 0.14.em,
                fontFamily = JetBrainsMono,
            )
        }
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
            content = extra,
        )
    }
}

// ----------------------------------------------------------------------------
// ThemeToggle
// ----------------------------------------------------------------------------
/**
 * Two-pill DARK / LIGHT switcher styled to match the design's mode toggle.
 * Designed to fit in [TopBar]'s right slot.
 */
@Composable
fun ThemeToggle(
    isDark: Boolean,
    onToggle: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val t = LocalTactical.current
    Row(
        modifier = modifier
            .border(1.dp, t.line)
            .background(t.panel)
            .clickable(onClick = onToggle),
    ) {
        TogglePill(text = "◐ DARK",  active = isDark)
        TogglePill(text = "◑ LIGHT", active = !isDark)
    }
}

@Composable
private fun TogglePill(text: String, active: Boolean) {
    val t = LocalTactical.current
    Box(
        modifier = Modifier
            .background(if (active) t.panelHi else Color.Transparent)
            .padding(horizontal = 10.dp, vertical = 4.dp)
    ) {
        Text(
            text = text,
            color = if (active) t.ink else t.inkDim,
            fontSize = 9.sp,
            letterSpacing = 0.18.em,
            fontFamily = JetBrainsMono,
        )
    }
}
