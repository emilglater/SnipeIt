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
 * To go to 6 mics: set NUM_CHANNELS to 6 in acoustic_config.h and add the
 * +/-60 degree pair at (-+0.0693, 0.04). Note the pair count goes from 6 to
 * 15, so gcc_phat_compute_all_pairs does 2.5x the FFT work per event.
 */
const mic_position_t MIC_POSITIONS[NUM_CHANNELS] = {
    {-0.0800, 0.0000},   // Mic 0: -90° (far left)
    {-0.0400, 0.0693},   // Mic 1: -30° (center-left)
    {+0.0400, 0.0693},   // Mic 2: +30° (center-right)
    {+0.0800, 0.0000},   // Mic 3: +90° (far right)
};
