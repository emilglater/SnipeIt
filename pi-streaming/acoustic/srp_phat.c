/**
 * srp_phat.c
 *
 * Steered Response Power with Phase Transform azimuth estimator.
 *
 * Implementation notes:
 *
 * 1. The expected TDOA for a plane wave arriving from azimuth theta
 *    is computed using the dot product of the microphone baseline
 *    vector with the unit direction vector:
 *
 *      tau_expected = (1/c) * (pos_b - pos_a) . u(theta)
 *
 *    where u(theta) = (sin(theta), cos(theta)) is the unit vector
 *    pointing toward the source. Here theta=0 is forward (+Y),
 *    positive theta is to the right (+X), matching the coordinate
 *    system defined in acoustic_config.h.
 *
 * 2. The power at each candidate angle sums, over all microphone pairs, a
 *    Gaussian kernel scoring the expected TDOA against that pair's measured
 *    TDOA, weighted by the pair's GCC-PHAT peak value. This is a reduced form
 *    of SRP-PHAT -- see srp_phat.h and the note in srp_phat_estimate().
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "srp_phat.h"

float srp_phat_compute_expected_tdoa(mic_position_t mic_a,
                                      mic_position_t mic_b,
                                      float azimuth_deg)
{
    /*
     * Direction unit vector for azimuth theta:
     *   u_x = sin(theta)   (positive = right)
     *   u_y = cos(theta)   (positive = forward)
     *
     * The wave propagates FROM the source TOWARD the array, which is
     * the opposite of u(theta). A microphone at position p receives
     * the signal at a relative time of: t = -p . u / c
     * (mics farther in the source direction receive the signal earlier).
     *
     * TDOA between mic_a and mic_b:
     *   tau = t_b - t_a = -(pos_b . u) / c + (pos_a . u) / c
     *       = -(pos_b - pos_a) . u / c
     *
     * Positive tau means mic_b receives the signal LATER than mic_a.
     */
    float theta_rad = DEG_TO_RAD(azimuth_deg);
    float ux = sinf(theta_rad);
    float uy = cosf(theta_rad);

    float dx = mic_b.x - mic_a.x;
    float dy = mic_b.y - mic_a.y;

    float tdoa = -(dx * ux + dy * uy) / SPEED_OF_SOUND;
    return tdoa;
}

void srp_phat_estimate(const gcc_phat_workspace_t *gcc_ws,
                        const float *multichannel,
                        int num_frames,
                        int sample_rate,
                        srp_phat_result_t *result)
{
    (void)multichannel;  /* Currently unused; reserved for future extensions */
    (void)num_frames;

    if (!gcc_ws || !result) return;

    memset(result, 0, sizeof(srp_phat_result_t));

    /*
     * Sweep over candidate azimuths from AZIMUTH_MIN_DEG to AZIMUTH_MAX_DEG.
     * theta accumulates in float. Exact at the current step of 1.0, since
     * -90.0, 1.0 and 90.0 are all exactly representable; a step that is not a
     * power of two can drift and drop the final angle.
     */
    int num_angles = 0;
    float best_power = -1e30f;
    float second_best_power = -1e30f;
    float best_azimuth = 0.0f;

    for (float theta = AZIMUTH_MIN_DEG; theta <= AZIMUTH_MAX_DEG; theta += AZIMUTH_STEP_DEG)
    {
        float power = 0.0f;

        /*
         * For each microphone pair, compute the expected TDOA at this
         * azimuth, convert to samples, and look up the correlation value.
         */
        for (int p = 0; p < gcc_ws->num_pairs; p++)
        {
            int mic_i = gcc_ws->pair_indices[p][0];
            int mic_j = gcc_ws->pair_indices[p][1];

            float expected_tdoa = srp_phat_compute_expected_tdoa(
                MIC_POSITIONS[mic_i], MIC_POSITIONS[mic_j], theta);

            float expected_delay_samples = expected_tdoa * (float)sample_rate;

            /*
             * Score the expected delay against this pair's measured TDOA with
             * a Gaussian kernel:
             *
             *   power += exp(-0.5 * ((expected_delay - measured_delay) / sigma)^2)
             *
             * True SRP-PHAT would look up the correlation function itself at
             * expected_delay, but gcc_phat_compute_all_pairs reuses one
             * correlation buffer across pairs, so only the last pair's survives
             * -- we have each pair's argmax and peak value, not its curve. This
             * matches SRP-PHAT closely when each pair's peak is clean.
             */
            float measured_tdoa_samples = gcc_ws->tdoa_results[p] * (float)sample_rate;
            float diff = expected_delay_samples - measured_tdoa_samples;

            /*
             * Gaussian kernel width (sigma) in samples.
             * A sigma of 1.0 means delays within ±1 sample of the expected
             * value contribute strongly. This matches the typical GCC-PHAT
             * peak width for impulsive signals at 48 kHz.
             */
            float sigma = 1.0f;
            float weight = expf(-0.5f * (diff * diff) / (sigma * sigma));

            /*
             * Scale by the pair's peak correlation value, so pairs with
             * stronger signals contribute more to the power estimate.
             */
            power += weight * gcc_ws->peak_values[p];
        }

        if (num_angles < 361)
        {
            result->power_spectrum[num_angles] = power;
        }
        num_angles++;

        /* Track the best and second-best peaks */
        if (power > best_power)
        {
            second_best_power = best_power;
            best_power = power;
            best_azimuth = theta;
        }
        else if (power > second_best_power)
        {
            second_best_power = power;
        }
    }

    result->num_angles_tested = num_angles;
    result->azimuth_deg       = best_azimuth;
    result->peak_power        = best_power;

    /*
     * Confidence based on peak sharpness vs the mean of the angular
     * power spectrum. This works for any number of mic pairs, including
     * the 2-mic case where the second-best peak is always ~= the best peak
     * because the spectrum is a smooth single Gaussian.
     *
     * If the peak is much higher than the mean, the source direction is
     * clearly defined → high confidence.
     * If the spectrum is flat, all directions are equally likely → low confidence.
     */
    float total_power = 0.0f;
    for (int i = 0; i < num_angles; i++)
    {
        total_power += result->power_spectrum[i];
    }
    float mean_power = total_power / (float)num_angles;

    if (best_power > 1e-6f && mean_power > 0.0f)
    {
        result->confidence = 1.0f - (mean_power / best_power);
    }
    else if (best_power > 1e-6f)
    {
        result->confidence = 1.0f;
    }
    else
    {
        result->confidence = 0.0f;
    }

    /* Clamp confidence to [0, 1] */
    if (result->confidence < 0.0f) result->confidence = 0.0f;
    if (result->confidence > 1.0f) result->confidence = 1.0f;
}
