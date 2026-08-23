package com.example.mysnipeit.ui.theme

import androidx.compose.ui.graphics.Color

/**
 * Tactical palette — final consolidated tokens from the SnipeIt redesign.
 * Low-emission, night-safe military aesthetic. Same role names across
 * modes; values swap via [TacticalPalette].
 *
 * WCAG AA verified by the designer:
 * - dark.ink on dark.base  ≈ 11:1 (AAA)
 * - dark.inkDim on dark.base ≈ 6.2:1 (AA / AAA-large)
 * - dark.accent (copper) ≈ 6.4:1 (AA)
 * - dark.danger ≈ 5.2:1 (AA-large)
 * - light variants: ink ≈ 13:1, inkDim ≈ 8:1, accent ≈ 7:1
 */
data class TacticalPalette(
    val base: Color,         // background
    val panel: Color,        // card / panel surface
    val panelHi: Color,      // selected / highlighted panel
    val line: Color,         // hairline border
    val lineHi: Color,       // active / strong border
    val ink: Color,          // primary text (bone / khaki)
    val inkDim: Color,       // secondary text
    val inkMute: Color,      // tertiary / non-essential
    val accent: Color,       // copper — active / warning state
    val accentDim: Color,    // dimmed accent
    val on: Color,           // muted olive — OK / connected
    val danger: Color,       // muted red
    val bboxTracked: Color,  // orange — tracked/unlocked bbox on the video
    val bboxLocked: Color,   // red — locked-target bbox on the video
    val videoChrome: Color,  // semi-transparent panel for HUD overlays on video
)

/**
 * Dark mode (primary). Hex values copied verbatim from the design's
 * `screens-final.jsx` D{} token block.
 */
val TacticalDark = TacticalPalette(
    base        = Color(0xFF08090A),
    panel       = Color(0xFF15181A),
    panelHi     = Color(0xFF20252A),
    line        = Color(0xFF2E332F),
    lineHi      = Color(0xFF4A504A),
    ink         = Color(0xFFD4CDBB),
    inkDim      = Color(0xFF9A9282),
    inkMute     = Color(0xFF6E6858),
    accent      = Color(0xFFC08850),
    accentDim   = Color(0xFF6A4A28),
    on          = Color(0xFF9AAE72),
    danger      = Color(0xFFC47668),
    // Bbox colors are always rendered over the live video (which is always dark
    // — real camera feed). Same hex values in both palettes so they read the
    // same regardless of UI mode.
    bboxTracked = Color(0xFFFF8C2D),  // bright orange — visible, distinct from copper accent
    bboxLocked  = Color(0xFFE6312E),  // vivid red — high alert, clearly different from orange
    videoChrome = Color(0xE015181A),  // rgba(21,24,26,0.88)
)

/**
 * Light mode. Hex values from `screens-final.jsx` Lt{} block.
 * The user explicitly said they may tweak these later — edit here.
 */
val TacticalLight = TacticalPalette(
    base        = Color(0xFFD4CDBA),
    panel       = Color(0xFFBDB6A0),
    panelHi     = Color(0xFFA89F86),
    line        = Color(0xFF6E6850),
    lineHi      = Color(0xFF3A3628),
    ink         = Color(0xFF1A1C14),
    inkDim      = Color(0xFF3E3A2C),
    inkMute     = Color(0xFF5E584A),
    accent      = Color(0xFF6A3A10),
    accentDim   = Color(0xFF8A5A28),
    on          = Color(0xFF2E3A18),
    danger      = Color(0xFF6A1F18),
    // Same bbox colors as dark mode — they sit on top of the live video which
    // is always dark, so the contrast target is the camera feed, not the UI.
    bboxTracked = Color(0xFFFF8C2D),
    bboxLocked  = Color(0xFFE6312E),
    videoChrome = Color(0xEBBDB6A0),  // rgba(189,182,160,0.92)
)

// ----------------------------------------------------------------------------
// Backwards-compat aliases
//
// The pre-redesign screens still reference these names. They map to the new
// dark-mode tactical tokens so the project keeps building during the
// screen-by-screen migration. Every reference to these names will disappear
// as the corresponding screen is rebuilt; the aliases themselves get deleted
// in a final cleanup commit.
// ----------------------------------------------------------------------------
@Deprecated("Use LocalTactical.current.base", ReplaceWith("LocalTactical.current.base"))
val MilitaryDarkBackground: Color = TacticalDark.base
@Deprecated("Use LocalTactical.current.panel", ReplaceWith("LocalTactical.current.panel"))
val MilitaryCardBackground: Color = TacticalDark.panel
@Deprecated("Use LocalTactical.current.on for status, .accent for active", ReplaceWith("LocalTactical.current.on"))
val MilitaryAccentGreen: Color = TacticalDark.on
@Deprecated("Use LocalTactical.current.accent", ReplaceWith("LocalTactical.current.accent"))
val MilitaryWarningAmber: Color = TacticalDark.accent
@Deprecated("Use LocalTactical.current.danger", ReplaceWith("LocalTactical.current.danger"))
val MilitaryDangerRed: Color = TacticalDark.danger
@Deprecated("Use LocalTactical.current.ink", ReplaceWith("LocalTactical.current.ink"))
val MilitaryTextPrimary: Color = TacticalDark.ink
@Deprecated("Use LocalTactical.current.inkDim", ReplaceWith("LocalTactical.current.inkDim"))
val MilitaryTextSecondary: Color = TacticalDark.inkDim
@Deprecated("Use LocalTactical.current.line", ReplaceWith("LocalTactical.current.line"))
val MilitaryBorderColor: Color = TacticalDark.line

@Deprecated("Use LocalTactical.current.on", ReplaceWith("LocalTactical.current.on"))
val StatusConnected: Color = TacticalDark.on
@Deprecated("Use LocalTactical.current.danger", ReplaceWith("LocalTactical.current.danger"))
val StatusDisconnected: Color = TacticalDark.danger
@Deprecated("Use LocalTactical.current.accent", ReplaceWith("LocalTactical.current.accent"))
val StatusConnecting: Color = TacticalDark.accent
@Deprecated("Use LocalTactical.current.danger", ReplaceWith("LocalTactical.current.danger"))
val StatusError: Color = TacticalDark.danger

@Deprecated("Use LocalTactical.current.accent", ReplaceWith("LocalTactical.current.accent"))
val TargetActive: Color = TacticalDark.accent
@Deprecated("Use LocalTactical.current.line", ReplaceWith("LocalTactical.current.line"))
val TargetInactive: Color = TacticalDark.line
@Deprecated("Use LocalTactical.current.accent", ReplaceWith("LocalTactical.current.accent"))
val TargetSelected: Color = TacticalDark.accent
