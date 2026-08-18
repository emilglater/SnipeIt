/**
 * srp_phat.h
 *
 * Steered Response Power with Phase Transform (SRP-PHAT)
 * for azimuth estimation from TDOA measurements.
 *
 * Evaluates a power function over a grid of candidate source directions. For
 * each candidate azimuth this computes the expected TDOA for every microphone
 * pair and scores how close it is to that pair's MEASURED TDOA, using a
 * Gaussian kernel one sample wide, weighted by the pair's GCC-PHAT peak value.
 * Summing over pairs gives the power at that angle; the highest wins.
 *
 * This is a reduced form of SRP-PHAT. True SRP-PHAT sums the correlation
 * function itself at the expected lag, which requires keeping every pair's
 * full correlation buffer; we keep only each pair's argmax. So a pair whose
 * peak-picking landed on the wrong lobe contributes a confidently wrong score
 * rather than a broad one.
 *
 * Combining all pairs is still steadier than trusting any single pair's peak:
 * a bad pair is outvoted, and the shape of the resulting curve gives a
 * confidence number for free.
 */

#ifndef SRP_PHAT_H
#define SRP_PHAT_H

#include "acoustic_config.h"
#include "gcc_phat.h"

/**
 * SRP-PHAT result structure.
 */
typedef struct
{
    float   azimuth_deg;        /* Best azimuth in degrees (-90 to +90) */
    float   confidence;         /* 0.0 to 1.0 */
    float   peak_power;         /* Raw SRP-PHAT power at best azimuth */
    float   power_spectrum[361]; /* Power at each tested angle. Sized for 361;
                                    the current sweep (-90..+90 step 1) uses 181.
                                    Invariant: (AZIMUTH_MAX_DEG - AZIMUTH_MIN_DEG)
                                    / AZIMUTH_STEP_DEG + 1 must stay <= 361. */
    int     num_angles_tested;  /* Number of angles in the power_spectrum */
} srp_phat_result_t;

/**
 * srp_phat_estimate - Estimate the source azimuth using SRP-PHAT.
 *
 * @gcc_ws:       The GCC-PHAT workspace, already populated by a call to
 *                gcc_phat_compute_all_pairs(). Reads pair_indices[],
 *                tdoa_results[], peak_values[] and num_pairs. The correlation
 *                scratch buffer is NOT used, so it may be overwritten between
 *                the two calls.
 * @multichannel: The same multichannel audio passed to gcc_phat.
 *                Not used; kept for potential future use.
 * @num_frames:   Number of audio frames. Not used.
 * @sample_rate:  Sample rate in Hz.
 * @result:       Output structure filled with the estimated azimuth,
 *                confidence, and the full power spectrum.
 *
 * The function sweeps azimuth from AZIMUTH_MIN_DEG to AZIMUTH_MAX_DEG
 * in steps of AZIMUTH_STEP_DEG, computing SRP-PHAT power at each.
 *
 * Confidence = 1 - (mean power / peak power) across the swept angles. A sharp
 * peak against a low background gives a value near 1; a flat spectrum gives
 * near 0. It is not a probability. See srp_phat.c for why this replaced a
 * peak-versus-second-peak measure.
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
 * The plane-wave assumption holds when the source is far compared to the
 * array. Ours is 16 cm across and targets are metres away, so it holds easily.
 */
float srp_phat_compute_expected_tdoa(mic_position_t mic_a,
                                      mic_position_t mic_b,
                                      float azimuth_deg);

#endif /* SRP_PHAT_H */
