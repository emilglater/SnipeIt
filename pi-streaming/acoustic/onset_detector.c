/**
 * onset_detector.c
 *
 * Real-time impulsive event detector.
 *
 * The high-pass filter is a 2nd-order Butterworth design, computed
 * using the bilinear transform. This is a standard IIR filter topology
 * that is efficient to compute (5 multiply-adds per sample).
 *
 * The energy tracking uses exponential moving averages (EMAs) rather
 * than true windowed sums. EMAs are computationally cheaper (one multiply-
 * add per sample vs. maintaining a sliding window) and respond smoothly
 * to energy changes. The time constant is set so that the EMA approximates
 * a rectangular window of the specified duration.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "onset_detector.h"

/* ---------------------------------------------------------------------------
 * Internal: Compute 2nd-order Butterworth high-pass filter coefficients.
 *
 * The Butterworth filter is chosen because it has maximally flat passband
 * response, meaning it does not distort the gunshot signal shape in the
 * passband. The bilinear transform maps the analog prototype to digital.
 *
 * Transfer function:
 *   H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
 * ------------------------------------------------------------------------ */
static void compute_highpass_coefficients(float cutoff_hz, int sample_rate,
                                           float *b0, float *b1, float *b2,
                                           float *a1, float *a2)
{
    /* Pre-warp the cutoff frequency for the bilinear transform */
    float omega = 2.0f * (float)M_PI * cutoff_hz / (float)sample_rate;
    float alpha = sinf(omega) / (2.0f * 0.7071f);  /* Q = 0.7071 for Butterworth */

    float cos_omega = cosf(omega);

    /* Unnormalized coefficients */
    float norm = 1.0f + alpha;

    *b0 = (1.0f + cos_omega) / (2.0f * norm);
    *b1 = -(1.0f + cos_omega) / norm;
    *b2 = (1.0f + cos_omega) / (2.0f * norm);
    *a1 = (-2.0f * cos_omega) / norm;
    *a2 = (1.0f - alpha) / norm;
}

onset_detector_t *onset_detector_create(int sample_rate, float cutoff_hz,
                                         float threshold, int refractory_ms)
{
    onset_detector_t *det = (onset_detector_t *)calloc(1, sizeof(onset_detector_t));
    if (!det)
    {
        fprintf(stderr, "[onset_detector] Failed to allocate detector\n");
        return NULL;
    }

    det->sample_rate = sample_rate;
    det->threshold   = threshold;

    /* Convert refractory period from ms to samples */
    det->refractory_samples  = (sample_rate * refractory_ms) / 1000;
    det->samples_since_trigger = det->refractory_samples;  /* Start ready to trigger */
    det->triggered = 0;

    /* Compute high-pass filter coefficients */
    compute_highpass_coefficients(cutoff_hz, sample_rate,
                                  &det->hp_b0, &det->hp_b1, &det->hp_b2,
                                  &det->hp_a1, &det->hp_a2);

    /* Initialize filter state to zero */
    memset(&det->hp_filter, 0, sizeof(biquad_state_t));

    /*
     * Compute EMA coefficients.
     *
     * For an EMA that approximates a rectangular window of duration T,
     * the coefficient alpha = 1 - exp(-1 / (T * sample_rate)).
     * When alpha is close to 0, the EMA is very smooth (long window).
     * When alpha is close to 1, the EMA tracks the input closely (short window).
     */
    float short_window_sec = (float)SHORT_WINDOW_MS / 1000.0f;
    float long_window_sec  = (float)LONG_WINDOW_MS  / 1000.0f;

    det->alpha_short = 1.0f - expf(-1.0f / (short_window_sec * (float)sample_rate));
    det->alpha_long  = 1.0f - expf(-1.0f / (long_window_sec  * (float)sample_rate));

    det->short_energy = 0.0f;
    det->long_energy  = 0.0f;

    /*
     * Warmup period: do not allow triggering until the long-term EMA
     * has had time to stabilize. Set to 1.5x the long window duration.
     */
    det->warmup_samples   = (int)(1.5f * long_window_sec * (float)sample_rate);
    det->total_samples    = 0;

    /*
     * Minimum energy floor: the long-term energy must exceed this value
     * before the ratio test is applied. This prevents false triggers
     * when the environment is nearly silent and the ratio is unstable.
     * This value corresponds to roughly -60 dBFS noise floor.
     */
    det->min_energy_floor = 1e-8f;

    return det;
}

void onset_detector_destroy(onset_detector_t *det)
{
    free(det);  /* free(NULL) is safe */
}

int onset_detector_process(onset_detector_t *det, const float *samples, int num_samples)
{
    if (!det || !samples || num_samples <= 0) return 0;

    int trigger_fired = 0;

    for (int i = 0; i < num_samples; i++)
    {
        /*
         * Step 1: Apply the high-pass biquad filter.
         *
         * Direct Form I implementation:
         *   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
         */
        float x = samples[i];
        float y = det->hp_b0 * x
                 + det->hp_b1 * det->hp_filter.x1
                 + det->hp_b2 * det->hp_filter.x2
                 - det->hp_a1 * det->hp_filter.y1
                 - det->hp_a2 * det->hp_filter.y2;

        /* Shift filter state */
        det->hp_filter.x2 = det->hp_filter.x1;
        det->hp_filter.x1 = x;
        det->hp_filter.y2 = det->hp_filter.y1;
        det->hp_filter.y1 = y;

        /*
         * Step 2: Compute instantaneous energy (squared amplitude).
         * Update both short-term and long-term EMAs.
         */
        float energy = y * y;

        det->short_energy += det->alpha_short * (energy - det->short_energy);
        det->long_energy  += det->alpha_long  * (energy - det->long_energy);

        /*
         * Step 3: Check trigger condition.
         * Only trigger if:
         *   - The energy ratio exceeds the threshold.
         *   - We are past the refractory period.
         *   - The long-term energy is above a minimum floor (to avoid
         *     triggering on near-silence where ratio is unstable).
         */
        det->samples_since_trigger++;
        det->total_samples++;

        float ratio = 0.0f;
        if (det->long_energy > 1e-12f)
        {
            ratio = det->short_energy / det->long_energy;
        }

        if (ratio > det->threshold &&
            det->samples_since_trigger >= det->refractory_samples &&
            det->total_samples >= det->warmup_samples &&
            det->long_energy > det->min_energy_floor)
        {
            trigger_fired = 1;
            det->samples_since_trigger = 0;
            det->triggered = 1;
        }
    }

    return trigger_fired;
}

void onset_detector_reset(onset_detector_t *det)
{
    if (!det) return;

    memset(&det->hp_filter, 0, sizeof(biquad_state_t));
    det->short_energy          = 0.0f;
    det->long_energy           = 0.0f;
    det->samples_since_trigger = det->refractory_samples;
    det->triggered             = 0;
    det->total_samples         = 0;
}

float onset_detector_get_energy_ratio(const onset_detector_t *det)
{
    if (!det || det->long_energy < 1e-12f) return 0.0f;
    return det->short_energy / det->long_energy;
}
