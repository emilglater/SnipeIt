/**
 * acoustic_config.c
 *
 * Defines the microphone positions for the semicircular array.
 *
 * The array is a forward-facing semicircle of radius ARRAY_RADIUS_M.
 * Coordinate system:
 *   +X = right
 *   +Y = forward (camera boresight)
 *   Origin = center of the semicircle
 *
 * For 4 microphones at -90, -30, +30, +90 degrees on the semicircle:
 *   Angle is measured from the forward (+Y) axis, positive clockwise.
 *
 *   Mic at angle theta:
 *     x = R * sin(theta)
 *     y = R * cos(theta)
 */

#include "acoustic_config.h"

/*
 * Microphone positions for a 4-mic semicircular array.
 *
 * If you upgrade to 6 microphones, change NUM_CHANNELS to 6 in
 * acoustic_config.h and update this array to:
 *
 * const mic_position_t MIC_POSITIONS[6] = {
 *     { -0.08f,   0.0f    },   // Mic 0: -90 deg
 *     { -0.0693f, 0.04f   },   // Mic 1: -60 deg
 *     { -0.04f,   0.0693f },   // Mic 2: -30 deg
 *     { +0.04f,   0.0693f },   // Mic 3: +30 deg
 *     { +0.0693f, 0.04f   },   // Mic 4: +60 deg
 *     { +0.08f,   0.0f    },   // Mic 5: +90 deg
 * };
 */

/* two mics setup
const mic_position_t MIC_POSITIONS[NUM_CHANNELS] = {
    {-0.0135, 0.0000},  // Mic 0 (Left)  — 1.35 cm left of center
    {+0.0135, 0.0000},  // Mic 1 (Right) — 1.35 cm right of center
};
*/
const mic_position_t MIC_POSITIONS[NUM_CHANNELS] = {
    {-0.0800, 0.0000},   // Mic 0: -90° (far left)
    {-0.0400, 0.0693},   // Mic 1: -30° (center-left)
    {+0.0400, 0.0693},   // Mic 2: +30° (center-right)
    {+0.0800, 0.0000},   // Mic 3: +90° (far right)
};
