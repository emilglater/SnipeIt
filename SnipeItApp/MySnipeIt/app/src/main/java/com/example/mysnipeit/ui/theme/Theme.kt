package com.example.mysnipeit.ui.theme

import android.app.Activity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

/**
 * The active tactical palette for the current composition. Screens read it
 * via `LocalTactical.current` so they don't need to take a [TacticalPalette]
 * parameter from every parent.
 */
val LocalTactical = staticCompositionLocalOf<TacticalPalette> { TacticalDark }

/**
 * Tracks whether the app is currently rendering in dark mode. Screens use this
 * for content that needs a mode-aware decision beyond the palette (e.g. the
 * map screen choosing between dark/light Google Maps JSON).
 */
val LocalIsDarkTheme = compositionLocalOf { true }

/**
 * Map [TacticalPalette] tokens to a Material3 [androidx.compose.material3.ColorScheme]
 * so any code that uses `MaterialTheme.colorScheme` automatically picks up the
 * tactical look. Tokens map to the closest Material role.
 */
private fun TacticalPalette.toColorScheme(isDark: Boolean) =
    if (isDark) darkColorScheme(
        primary       = accent,
        onPrimary     = base,
        secondary     = on,
        onSecondary   = base,
        tertiary      = inkDim,
        background    = base,
        onBackground  = ink,
        surface       = panel,
        onSurface     = ink,
        surfaceVariant = panelHi,
        onSurfaceVariant = inkDim,
        outline       = line,
        outlineVariant = lineHi,
        error         = danger,
        onError       = base,
    ) else lightColorScheme(
        primary       = accent,
        onPrimary     = ink,
        secondary     = on,
        onSecondary   = ink,
        tertiary      = inkDim,
        background    = base,
        onBackground  = ink,
        surface       = panel,
        onSurface     = ink,
        surfaceVariant = panelHi,
        onSurfaceVariant = inkDim,
        outline       = line,
        outlineVariant = lineHi,
        error         = danger,
        onError       = base,
    )

/**
 * Root theme entry. Pass `darkTheme` from app state (persisted in
 * SniperViewModel or DataStore) to drive the toggle.
 */
@Composable
fun MySniperItTheme(
    darkTheme: Boolean = true,
    content: @Composable () -> Unit
) {
    val palette = if (darkTheme) TacticalDark else TacticalLight
    val colorScheme = palette.toColorScheme(darkTheme)

    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            window.statusBarColor = palette.base.toArgb()
            WindowCompat.getInsetsController(window, view)
                .isAppearanceLightStatusBars = !darkTheme
        }
    }

    androidx.compose.runtime.CompositionLocalProvider(
        LocalTactical provides palette,
        LocalIsDarkTheme provides darkTheme,
    ) {
        MaterialTheme(
            colorScheme = colorScheme,
            typography = Typography,
            content = content
        )
    }
}
