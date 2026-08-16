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
 * High-pass filter state for a 2nd-order IIR (biquad) filter.
 * Two past input samples and two past output samples.
 */
typedef struct
{
    float x1, x2;      /* Previous input samples */
    float y1, y2;      /* Previous output samples */
} biquad_state_t;

/**
 * Main onset detector state.
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
    int             samples_since_trigger;  /* Counter since last trigger */
    int             triggered;              /* Flag: 1 if currently in triggered state */
    int             warmup_samples;         /* Samples needed before allowing triggers */
    int             total_samples;          /* Total samples processed since reset */
    float           min_energy_floor;       /* Minimum long-term energy to allow trigger */

    /* Configuration */
    int             sample_rate;
} onset_detector_t;

/**
 * onset_detector_create - Allocate and initialize an onset detector.
 *
 * @sample_rate:    Audio sample rate (e.g., 48000).
 * @cutoff_hz:      High-pass filter cutoff frequency (e.g., 300.0).
 * @threshold:      Short/long energy ratio to trigger (e.g., 10.0).
 * @refractory_ms:  Minimum milliseconds between triggers (e.g., 500).
 *
 * Returns a pointer to the new detector, or NULL on failure.
 */
onset_detector_t *onset_detector_create(int sample_rate, float cutoff_hz,
                                         float threshold, int refractory_ms);

/**
 * onset_detector_destroy - Free all memory.
 */
void onset_detector_destroy(onset_detector_t *det);

/**
 * onset_detector_process - Feed a chunk of mono audio and check for trigger.
 *
 * @det:        The onset detector.
 * @samples:    Array of mono float samples (already downmixed from
 *              multichannel if needed). Values should be normalized
 *              to approximately [-1.0, 1.0].
 * @num_samples: Number of samples in the chunk.
 *
 * Returns 1 if a trigger was fired during this chunk, 0 otherwise.
 * If triggered, the detector enters the refractory period automatically.
 */
int onset_detector_process(onset_detector_t *det, const float *samples, int num_samples);

/**
 * onset_detector_reset - Reset the detector state (filter, energy, trigger).
 * Useful when changing environments or after reconfiguration.
 */
void onset_detector_reset(onset_detector_t *det);

/**
 * onset_detector_get_energy_ratio - Return the current short/long energy
 * ratio. Useful for debugging and visualization.
 */
float onset_detector_get_energy_ratio(const onset_detector_t *det);

#endif /* ONSET_DETECTOR_H */
