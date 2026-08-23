package com.example.mysnipeit.ui.theme

import androidx.compose.runtime.Composable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/**
 * Cross-screen responsive breakpoint helpers.
 *
 * The redesign was sized for a 1280x800 tablet in landscape. Phones in
 * landscape are typically 640-800 dp wide, which makes the fixed-width
 * panels (intel pane, side rails, firing-solution card) eat far too
 * much horizontal space. Screens that have a "compact" alternative use
 * [isCompactWidth] to decide.
 *
 * Threshold of 840 dp is chosen empirically: it sits above every
 * phone-landscape width we expect to see and below every tablet-landscape
 * width. Adjust here if you ever want to widen the "phone" bucket.
 */
private const val COMPACT_WIDTH_THRESHOLD_DP = 840

/** Returns true when the current screen width is in the phone/compact bucket. */
@Composable
@ReadOnlyComposable
fun isCompactWidth(): Boolean =
    LocalConfiguration.current.screenWidthDp < COMPACT_WIDTH_THRESHOLD_DP

/** Picks between a tablet-sized value and a compact-sized one in one call. */
@Composable
@ReadOnlyComposable
fun responsiveDp(tablet: Dp, compact: Dp): Dp =
    if (isCompactWidth()) compact else tablet

/** Same idea, but for plain Int/Float values (e.g. when picking a flex weight). */
@Composable
@ReadOnlyComposable
fun <T> responsive(tablet: T, compact: T): T =
    if (isCompactWidth()) compact else tablet
