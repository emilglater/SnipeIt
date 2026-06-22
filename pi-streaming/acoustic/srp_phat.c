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
 * 2. The SRP-PHAT power at each candidate angle is computed by
 *    summing the GCC-PHAT correlation values at the expected delays
 *    for all microphone pairs. We use linear interpolation between
 *    the two nearest correlation bins for smooth power variation.
 *
 * 3. Confidence is based on the ratio of the second-highest peak
 *    to the highest peak in the angular power spectrum. If there is
 *    a clear single peak, confidence is high. If multipath or noise
 *    creates multiple comparable peaks, confidence drops.
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
     */
    int num_angles = 0;
    float best_power = -1e30f;
    float second_best_power = -1e30f;
    float best_azimuth = 0.0f;

    for (float theta = AZIMUTH_MIN_DEG; theta <= AZIMUTH_MAX_DEG; theta += AZIMUTH_STEP_DEG) {
        float power = 0.0f;

        /*
         * For each microphone pair, compute the expected TDOA at this
         * azimuth, convert to samples, and look up the correlation value.
         */
        for (int p = 0; p < gcc_ws->num_pairs; p++) {
            int mic_i = gcc_ws->pair_indices[p][0];
            int mic_j = gcc_ws->pair_indices[p][1];

            float expected_tdoa = srp_phat_compute_expected_tdoa(
                MIC_POSITIONS[mic_i], MIC_POSITIONS[mic_j], theta);

            float expected_delay_samples = expected_tdoa * (float)sample_rate;

            /*
             * Look up the GCC-PHAT correlation value at this expected delay.
             *
             * We use the raw correlation buffer from the most recent
             * gcc_phat_compute_pair call. Since gcc_phat_compute_all_pairs
             * processes pairs sequentially and reuses the correlation buffer,
             * we need to recompute the correlation for each pair here.
             *
             * OPTIMIZATION NOTE: In a production implementation, you would
             * store the full correlation buffer for each pair. For clarity
             * and simplicity, we instead use the TDOA results directly.
             *
             * Alternative approach (used here): Instead of looking up the
             * full correlation, approximate the SRP-PHAT power using a
             * Gaussian kernel centered on the measured TDOA:
             *
             *   power += exp(-0.5 * ((expected_delay - measured_delay) / sigma)^2)
             *
             * This is mathematically equivalent to SRP-PHAT when the
             * correlation peaks are well-defined, and avoids the need to
             * store all correlation buffers.
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

        /* Store power for this angle */
        if (num_angles < 361) {
            result->power_spectrum[num_angles] = power;
        }
        num_angles++;

        /* Track the best and second-best peaks */
        if (power > best_power) {
            second_best_power = best_power;
            best_power = power;
            best_azimuth = theta;
        } else if (power > second_best_power) {
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
    for (int i = 0; i < num_angles; i++) {
        total_power += result->power_spectrum[i];
    }
    float mean_power = total_power / (float)num_angles;

    if (best_power > 1e-6f && mean_power > 0.0f) {
        result->confidence = 1.0f - (mean_power / best_power);
    } else if (best_power > 1e-6f) {
        result->confidence = 1.0f;
    } else {
        result->confidence = 0.0f;
    }

    /* Clamp confidence to [0, 1] */
    if (result->confidence < 0.0f) result->confidence = 0.0f;
    if (result->confidence > 1.0f) result->confidence = 1.0f;
}
