/**
 * gcc_phat.h
 *
 * Generalized Cross-Correlation with Phase Transform (GCC-PHAT)
 * for Time Delay of Arrival (TDOA) estimation.
 *
 * This is the core signal processing algorithm used in the IEEE paper
 * (Abiri & Pourmohammad, 2020). GCC-PHAT computes the cross-correlation
 * between two microphone signals with a normalization (PHAT weighting)
 * that whitens the spectrum. This produces a very sharp correlation peak,
 * making it ideal for impulsive broadband signals like gunshots.
 *
 * The algorithm:
 *   1. Compute FFT of both signals.
 *   2. Compute cross-power spectrum: R(f) = X1(f) * conj(X2(f)).
 *   3. Apply PHAT weighting: R_phat(f) = R(f) / |R(f)|.
 *   4. Compute IFFT of R_phat to get the cross-correlation.
 *   5. Find the peak of the cross-correlation -> time delay.
 *   6. Refine with parabolic interpolation for sub-sample precision.
 */

#ifndef GCC_PHAT_H
#define GCC_PHAT_H

#include <fftw3.h>
#include "acoustic_config.h"

/**
 * Pre-allocated workspace for GCC-PHAT computation.
 * Creating FFTW plans is expensive, so we do it once at initialization
 * and reuse the plans and scratch buffers for every computation.
 */
typedef struct
{
    int             fft_size;
    int             num_channels;

    /* FFTW plans and buffers (one per channel for forward FFT,
     * plus one for the inverse FFT of the cross-correlation) */
    float          *time_buf_a;          /* Input buffer for signal A */
    float          *time_buf_b;          /* Input buffer for signal B */
    fftwf_complex  *freq_buf_a;          /* FFT output for signal A */
    fftwf_complex  *freq_buf_b;          /* FFT output for signal B */
    fftwf_complex  *cross_spectrum;      /* Cross-spectrum after PHAT weighting */
    float          *correlation;         /* IFFT output: cross-correlation */

    fftwf_plan      plan_fwd_a;          /* Forward FFT plan for signal A */
    fftwf_plan      plan_fwd_b;          /* Forward FFT plan for signal B */
    fftwf_plan      plan_inv;            /* Inverse FFT plan for correlation */

    /* Results storage for all microphone pairs */
    float          *tdoa_results;        /* Array of TDOA values in seconds */
    float          *peak_values;         /* Peak correlation value (for confidence) */
    int             num_pairs;           /* Number of mic pairs = N*(N-1)/2 */

    /* Pair indexing: pair_indices[p][0] and pair_indices[p][1] give
     * the mic indices for pair p */
    int           (*pair_indices)[2];
} gcc_phat_workspace_t;

/**
 * gcc_phat_create - Allocate workspace and create FFTW plans.
 *
 * @fft_size:     FFT length (must be power of 2, e.g., 4096).
 * @num_channels: Number of microphones.
 *
 * Returns a pointer to the workspace, or NULL on failure.
 * FFTW plan creation is slow (~100 ms) so do this once at startup.
 */
gcc_phat_workspace_t *gcc_phat_create(int fft_size, int num_channels);

/**
 * gcc_phat_destroy - Free all workspace memory and FFTW plans.
 */
void gcc_phat_destroy(gcc_phat_workspace_t *ws);

/**
 * gcc_phat_compute_all_pairs - Compute TDOA for all microphone pairs.
 *
 * @ws:             The pre-allocated workspace.
 * @multichannel:   Interleaved multichannel audio data.
 *                  Layout: [f0_ch0, f0_ch1, ..., f1_ch0, f1_ch1, ...]
 * @num_frames:     Number of frames in the input (should be <= fft_size).
 * @sample_rate:    Sample rate for converting delay from samples to seconds.
 *
 * After this call:
 *   ws->tdoa_results[p] contains the TDOA in seconds for pair p.
 *   ws->peak_values[p]  contains the peak correlation magnitude for pair p.
 *
 * The TDOA sign convention:
 *   Positive TDOA means signal arrives at mic pair_indices[p][1] LATER
 *   than at mic pair_indices[p][0].
 */
void gcc_phat_compute_all_pairs(gcc_phat_workspace_t *ws,
                                 const float *multichannel,
                                 int num_frames,
                                 int sample_rate);

/**
 * gcc_phat_compute_pair - Compute TDOA for a single microphone pair.
 *
 * @ws:          The workspace.
 * @signal_a:    Mono audio from microphone A (num_frames samples).
 * @signal_b:    Mono audio from microphone B (num_frames samples).
 * @num_frames:  Number of samples in each signal.
 * @sample_rate: Sample rate in Hz.
 * @tdoa_out:    Output: estimated TDOA in seconds.
 * @peak_out:    Output: peak correlation value (confidence indicator).
 *
 * This is the low-level function; gcc_phat_compute_all_pairs calls it
 * internally for each pair.
 */
void gcc_phat_compute_pair(gcc_phat_workspace_t *ws,
                            const float *signal_a,
                            const float *signal_b,
                            int num_frames,
                            int sample_rate,
                            float *tdoa_out,
                            float *peak_out);

/**
 * gcc_phat_get_pair_index - Given two mic indices (i, j), return the
 * pair index p such that ws->tdoa_results[p] is the TDOA for that pair.
 *
 * Assumes i < j. Returns -1 if not found.
 */
int gcc_phat_get_pair_index(const gcc_phat_workspace_t *ws, int mic_i, int mic_j);

#endif /* GCC_PHAT_H */
