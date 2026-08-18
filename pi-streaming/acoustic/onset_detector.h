/**
 * onset_detector.h
 *
 * Real-time impulsive event detector for gunshot/explosion sounds.
 *
 * Algorithm overview:
 *   1. Apply a 2nd-order Butterworth high-pass filter at 300 Hz to
 *      reject wind noise and low-frequency environmental rumble.
 *   2. Compute short-term energy (10 ms window) and long-term energy
 *      (500 ms window) using exponential moving averages.
 *   3. When the ratio (short / long) exceeds a threshold, fire a trigger.
 *   4. Enforce a refractory period after each trigger to avoid
 *      re-triggering on echoes or the signal tail.
 *
 * The detector operates on a single channel (or the average of all
 * channels). It processes audio in small chunks (e.g., 10 ms) and
 * maintains state between calls.
 */

#ifndef ONSET_DETECTOR_H
#define ONSET_DETECTOR_H

#include "acoustic_config.h"

/**
 * @brief High-pass filter state for a 2nd-order IIR (biquad) filter.
 *
 * @details Two past input samples and two past output samples.
 */
typedef struct
{
    float x1, x2;      /* Previous input samples */
    float y1, y2;      /* Previous output samples */
} biquad_state_t;

/**
 * @brief Main onset detector state.
 */
typedef struct
{
    /* High-pass filter */
    biquad_state_t  hp_filter;
    float           hp_b0, hp_b1, hp_b2;    /* Numerator coefficients */
    float           hp_a1, hp_a2;            /* Denominator coefficients (a0 = 1) */

    /* Energy tracking */
    float           short_energy;           /* Exponential moving average, short window */
    float           long_energy;            /* Exponential moving average, long window */
    float           alpha_short;            /* EMA coefficient for short window */
    float           alpha_long;             /* EMA coefficient for long window */

    /* Trigger state */
    float           threshold;              /* Ratio threshold for triggering */
    int             refractory_samples;     /* Minimum samples between triggers */
    int64_t         samples_since_trigger;  /* Counter since last trigger */
    int             triggered;              /* Latches to 1 on the first trigger and
                                               stays set until reset(). Not a live
                                               state flag -- refractory status is
                                               samples_since_trigger vs
                                               refractory_samples. */
    int             warmup_samples;         /* Samples needed before allowing triggers */
    int64_t         total_samples;          /* Total samples processed since reset */
    float           min_energy_floor;       /* Minimum long-term energy to allow trigger */

    /* samples_since_trigger and total_samples must stay 64-bit. Both advance once
     * per sample and are only reset by a trigger (samples_since_trigger) or by
     * reset() (total_samples), so 32-bit versions wrap after ~12.4 h at 48 kHz.
     * Once negative, the refractory and warmup tests never pass again and the
     * detector stops firing without any error. */

    /* Configuration */
    int             sample_rate;
} onset_detector_t;

/**
 * @brief Allocate and initialize an onset detector.
 *
 * @param sample_rate   Audio sample rate (e.g., 48000).
 * @param cutoff_hz     High-pass filter cutoff frequency (e.g., 300.0).
 * @param threshold     Short/long energy ratio to trigger (e.g., 10.0).
 * @param refractory_ms Minimum milliseconds between triggers (e.g., 500).
 *
 * @details The two energy window lengths are NOT arguments: they come from
 *          SHORT_WINDOW_MS / LONG_WINDOW_MS at compile time. The detector also
 *          refuses to fire for the first 1.5 long-windows (0.75 s at the
 *          default 500 ms) while the long-term EMA settles, which looks like a
 *          broken detector if you test with a short recording.
 *
 * @returns A pointer to the new detector, or NULL on failure.
 */
onset_detector_t *onset_detector_create(int sample_rate, float cutoff_hz,
                                         float threshold, int refractory_ms);

/**
 * @brief Free all memory.
 *
 * @param det The detector to destroy. May be NULL (no-op).
 */
void onset_detector_destroy(onset_detector_t *det);

/**
 * @brief Feed a chunk of mono audio and check for a trigger.
 *
 * @param det         The onset detector.
 * @param samples     Array of mono float samples (already downmixed from
 *                    multichannel if needed). Values should be normalized to
 *                    approximately [-1.0, 1.0].
 * @param num_samples Number of samples in the chunk.
 *
 * @details If triggered, the detector enters the refractory period
 *          automatically.
 *
 * @returns 1 if a trigger fired during this chunk, 0 otherwise.
 */
int onset_detector_process(onset_detector_t *det, const float *samples, int num_samples);

/**
 * @brief Reset the detector state (filter, energy, trigger).
 *
 * @param det The onset detector.
 *
 * @details Useful when changing environments or after reconfiguration.
 */
void onset_detector_reset(onset_detector_t *det);

/**
 * @brief Return the current short/long energy ratio.
 *
 * @param det The onset detector.
 *
 * @returns The short/long energy ratio, or 0.0 when the long-term energy is
 *          below 1e-12 -- indistinguishable from a genuine ratio of zero.
 */
float onset_detector_get_energy_ratio(const onset_detector_t *det);

#endif /* ONSET_DETECTOR_H */
