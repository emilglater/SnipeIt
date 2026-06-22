/**
 * srp_phat.h
 *
 * Steered Response Power with Phase Transform (SRP-PHAT)
 * for azimuth estimation from TDOA measurements.
 *
 * SRP-PHAT works by evaluating a power function over a grid of
 * candidate source directions. For each candidate azimuth, it computes
 * the expected TDOA for every microphone pair, then looks up the
 * GCC-PHAT correlation at that delay. The sum across all pairs gives
 * the "steered response power" at that angle. The angle with the
 * highest power is the estimated source direction.
 *
 * This is more robust than simply taking the peak of individual
 * GCC-PHAT pairs because:
 *   - It integrates information across all pairs simultaneously.
 *   - A noisy estimate in one pair is compensated by clean estimates
 *     in other pairs.
 *   - It naturally produces a confidence measure (peak sharpness).
 */

#ifndef SRP_PHAT_H
#define SRP_PHAT_H

#include "acoustic_config.h"
#include "gcc_phat.h"

/**
 * SRP-PHAT result structure.
 */
typedef struct {
    float   azimuth_deg;        /* Best azimuth in degrees (-90 to +90) */
    float   confidence;         /* 0.0 to 1.0 */
    float   peak_power;         /* Raw SRP-PHAT power at best azimuth */
    float   power_spectrum[361]; /* Power at each tested angle (for debugging).
                                    Array size is generous; actual used portion
                                    depends on AZIMUTH_MIN/MAX/STEP. */
    int     num_angles_tested;  /* Number of angles in the power_spectrum */
} srp_phat_result_t;

/**
 * srp_phat_estimate - Estimate the source azimuth using SRP-PHAT.
 *
 * @gcc_ws:       The GCC-PHAT workspace, already populated by a call
 *                to gcc_phat_compute_all_pairs(). The correlation data
 *                in gcc_ws->correlation is used.
 * @multichannel: The same multichannel audio passed to gcc_phat.
 *                Not used directly here; kept for potential future use.
 * @num_frames:   Number of audio frames.
 * @sample_rate:  Sample rate in Hz.
 * @result:       Output structure filled with the estimated azimuth,
 *                confidence, and the full power spectrum.
 *
 * The function sweeps azimuth from AZIMUTH_MIN_DEG to AZIMUTH_MAX_DEG
 * in steps of AZIMUTH_STEP_DEG, computing SRP-PHAT power at each.
 *
 * Confidence is computed as:
 *   confidence = 1.0 - (second_peak / first_peak)
 * A single strong peak gives high confidence; multiple comparable
 * peaks (e.g., from multipath) give low confidence.
 */
void srp_phat_estimate(const gcc_phat_workspace_t *gcc_ws,
                        const float *multichannel,
                        int num_frames,
                        int sample_rate,
                        srp_phat_result_t *result);

/**
 * srp_phat_compute_expected_tdoa - Compute the expected TDOA between
 * two microphones for a plane wave arriving from a given azimuth.
 *
 * @mic_a:       Position of microphone A.
 * @mic_b:       Position of microphone B.
 * @azimuth_deg: Source azimuth in degrees (0 = forward, positive = right).
 *
 * Returns the expected TDOA in seconds (positive means signal arrives
 * at mic_b later than mic_a).
 *
 * The plane-wave (far-field) assumption is valid when the source is
 * much farther than the array aperture. For our array (16 cm diameter)
 * and minimum range (a few meters), this is well satisfied.
 */
float srp_phat_compute_expected_tdoa(mic_position_t mic_a,
                                      mic_position_t mic_b,
                                      float azimuth_deg);

#endif /* SRP_PHAT_H */
