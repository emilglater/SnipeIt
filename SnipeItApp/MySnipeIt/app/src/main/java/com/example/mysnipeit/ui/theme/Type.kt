package com.example.mysnipeit.ui.theme

import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.example.mysnipeit.R

/**
 * Tactical typography.
 *
 * - Inter (sans-serif) for UI / body text.
 * - JetBrains Mono for data, technical readouts, labels, big numerals.
 *
 * Both are bundled in `res/font/` so the app works offline (on the RPi AP
 * where there's no Internet to fetch downloadable fonts).
 */

val Inter = FontFamily(
    Font(R.font.inter_regular,  FontWeight.Normal),
    Font(R.font.inter_medium,   FontWeight.Medium),
    Font(R.font.inter_semibold, FontWeight.SemiBold),
)

val JetBrainsMono = FontFamily(
    Font(R.font.jetbrains_mono_regular,  FontWeight.Normal),
    Font(R.font.jetbrains_mono_medium,   FontWeight.Medium),
    Font(R.font.jetbrains_mono_semibold, FontWeight.SemiBold),
)

/**
 * Material3 Typography. Body/UI uses Inter; we override technical styles
 * (`displayLarge`, `labelSmall`, `labelMedium`) to JetBrains Mono so they
 * carry the tactical look by default.
 */
val Typography = Typography(
    displayLarge = TextStyle(
        fontFamily = JetBrainsMono,
        fontWeight = FontWeight.Normal,
        fontSize = 88.sp,
        letterSpacing = 0.02.em,
    ),
    headlineMedium = TextStyle(
        fontFamily = JetBrainsMono,
        fontWeight = FontWeight.Normal,
        fontSize = 22.sp,
        letterSpacing = 0.06.em,
    ),
    titleMedium = TextStyle(
        fontFamily = Inter,
        fontWeight = FontWeight.Medium,
        fontSize = 16.sp,
        lineHeight = 24.sp,
    ),
    bodyLarge = TextStyle(
        fontFamily = Inter,
        fontWeight = FontWeight.Normal,
        fontSize = 16.sp,
        lineHeight = 22.sp,
    ),
    bodyMedium = TextStyle(
        fontFamily = Inter,
        fontWeight = FontWeight.Normal,
        fontSize = 14.sp,
        lineHeight = 20.sp,
    ),
    labelMedium = TextStyle(
        // Used for mono data readouts like AZ/EL values
        fontFamily = JetBrainsMono,
        fontWeight = FontWeight.Normal,
        fontSize = 13.sp,
        letterSpacing = 0.06.em,
    ),
    labelSmall = TextStyle(
        // Used for the small uppercase "Lbl" labels everywhere in the design
        fontFamily = JetBrainsMono,
        fontWeight = FontWeight.Normal,
        fontSize = 10.sp,
        letterSpacing = 0.18.em,
    ),
)
