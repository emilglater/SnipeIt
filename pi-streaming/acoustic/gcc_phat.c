/**
 * gcc_phat.c
 *
 * Implementation of GCC-PHAT for TDOA estimation.
 *
 * Key implementation notes:
 *
 * 1. FFTW plans are created with FFTW_MEASURE during initialization.
 *    This takes longer (~100 ms) than FFTW_ESTIMATE but produces faster
 *    plans. Since we create plans only once, this is the right tradeoff.
 *
 * 2. The PHAT normalization divides the cross-spectrum by its magnitude.
 *    This effectively discards amplitude information and retains only phase,
 *    which is why it produces such sharp correlation peaks for impulsive
 *    signals. A small epsilon is added to avoid division by zero.
 *
 * 3. Parabolic interpolation around the correlation peak provides sub-sample
 *    delay precision. Without it, the delay resolution is limited to 1/fs
 *    (about 21 microseconds at 48 kHz). With interpolation, we achieve
 *    roughly 1/10 of a sample (~2 microseconds), which translates to
 *    sub-degree angular precision for our array geometry.
 *
 * 4. The correlation output of IFFT has the zero-lag at index 0, positive
 *    lags at indices 1..N/2-1, and negative lags at indices N/2..N-1
 *    (wrapped around). We search across both positive and negative lags
 *    up to the maximum physically possible delay (limited by mic spacing
 *    and speed of sound).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "gcc_phat.h"

/* ---------------------------------------------------------------------------
 * Internal: Parabolic interpolation around a peak for sub-sample precision.
 *
 * Given three consecutive values y[idx-1], y[idx], y[idx+1] where y[idx]
 * is the maximum, fits a parabola and returns the fractional offset of
 * the true peak relative to idx.
 *
 * Returns a value in [-0.5, +0.5] to add to idx.
 * ------------------------------------------------------------------------ */
static float parabolic_interpolation(const float *y, int idx, int len)
{
    /* Handle edge cases: if peak is at the boundary, no interpolation */
    int prev = (idx - 1 + len) % len;
    int next = (idx + 1) % len;

    float y_prev = y[prev];
    float y_curr = y[idx];
    float y_next = y[next];

    float denom = y_prev - 2.0f * y_curr + y_next;
    if (fabsf(denom) < 1e-12f) {
        return 0.0f;  /* Flat region, no interpolation possible */
    }

    float offset = 0.5f * (y_prev - y_next) / denom;

    /* Clamp to [-0.5, +0.5] for numerical safety */
    if (offset > 0.5f)  offset = 0.5f;
    if (offset < -0.5f) offset = -0.5f;

    return offset;
}

/* ---------------------------------------------------------------------------
 * Internal: Build the list of unique microphone pairs.
 * For N mics, generates N*(N-1)/2 pairs with i < j.
 * ------------------------------------------------------------------------ */
static void build_pair_indices(gcc_phat_workspace_t *ws)
{
    int p = 0;
    for (int i = 0; i < ws->num_channels; i++) {
        for (int j = i + 1; j < ws->num_channels; j++) {
            ws->pair_indices[p][0] = i;
            ws->pair_indices[p][1] = j;
            p++;
        }
    }
    ws->num_pairs = p;
}

gcc_phat_workspace_t *gcc_phat_create(int fft_size, int num_channels)
{
    gcc_phat_workspace_t *ws = (gcc_phat_workspace_t *)calloc(1, sizeof(gcc_phat_workspace_t));
    if (!ws) {
        fprintf(stderr, "[gcc_phat] Failed to allocate workspace\n");
        return NULL;
    }

    ws->fft_size     = fft_size;
    ws->num_channels = num_channels;

    /* Frequency-domain size for real FFT: N/2 + 1 complex values */
    int freq_size = fft_size / 2 + 1;

    /* Allocate FFTW buffers using fftwf_malloc for proper alignment */
    ws->time_buf_a    = (float *)fftwf_malloc(sizeof(float) * fft_size);
    ws->time_buf_b    = (float *)fftwf_malloc(sizeof(float) * fft_size);
    ws->freq_buf_a    = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * freq_size);
    ws->freq_buf_b    = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * freq_size);
    ws->cross_spectrum = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * freq_size);
    ws->correlation   = (float *)fftwf_malloc(sizeof(float) * fft_size);

    if (!ws->time_buf_a || !ws->time_buf_b || !ws->freq_buf_a ||
        !ws->freq_buf_b || !ws->cross_spectrum || !ws->correlation) {
        fprintf(stderr, "[gcc_phat] Failed to allocate FFTW buffers\n");
        gcc_phat_destroy(ws);
        return NULL;
    }

    /* Create FFTW plans */
    ws->plan_fwd_a = fftwf_plan_dft_r2c_1d(fft_size, ws->time_buf_a, ws->freq_buf_a, FFTW_ESTIMATE);
    ws->plan_fwd_b = fftwf_plan_dft_r2c_1d(fft_size, ws->time_buf_b, ws->freq_buf_b, FFTW_ESTIMATE);
    ws->plan_inv   = fftwf_plan_dft_c2r_1d(fft_size, ws->cross_spectrum, ws->correlation, FFTW_ESTIMATE);

    if (!ws->plan_fwd_a || !ws->plan_fwd_b || !ws->plan_inv) {
        fprintf(stderr, "[gcc_phat] Failed to create FFTW plans\n");
        gcc_phat_destroy(ws);
        return NULL;
    }

    /* Allocate pair-related arrays */
    int num_pairs = num_channels * (num_channels - 1) / 2;
    ws->tdoa_results  = (float *)calloc(num_pairs, sizeof(float));
    ws->peak_values   = (float *)calloc(num_pairs, sizeof(float));
    ws->pair_indices  = (int (*)[2])calloc(num_pairs, sizeof(int[2]));

    if (!ws->tdoa_results || !ws->peak_values || !ws->pair_indices) {
        fprintf(stderr, "[gcc_phat] Failed to allocate pair arrays\n");
        gcc_phat_destroy(ws);
        return NULL;
    }

    build_pair_indices(ws);

    return ws;
}

void gcc_phat_destroy(gcc_phat_workspace_t *ws)
{
    if (!ws) return;

    if (ws->plan_fwd_a)  fftwf_destroy_plan(ws->plan_fwd_a);
    if (ws->plan_fwd_b)  fftwf_destroy_plan(ws->plan_fwd_b);
    if (ws->plan_inv)    fftwf_destroy_plan(ws->plan_inv);

    if (ws->time_buf_a)    fftwf_free(ws->time_buf_a);
    if (ws->time_buf_b)    fftwf_free(ws->time_buf_b);
    if (ws->freq_buf_a)    fftwf_free(ws->freq_buf_a);
    if (ws->freq_buf_b)    fftwf_free(ws->freq_buf_b);
    if (ws->cross_spectrum) fftwf_free(ws->cross_spectrum);
    if (ws->correlation)   fftwf_free(ws->correlation);

    free(ws->tdoa_results);
    free(ws->peak_values);
    free(ws->pair_indices);

    free(ws);
}

void gcc_phat_compute_pair(gcc_phat_workspace_t *ws,
                            const float *signal_a,
                            const float *signal_b,
                            int num_frames,
                            int sample_rate,
                            float *tdoa_out,
                            float *peak_out)
{
    int N = ws->fft_size;
    int freq_size = N / 2 + 1;

    /*
     * Step 1: Copy input signals into FFTW buffers, zero-padding if
     * the input is shorter than fft_size.
     */
    memset(ws->time_buf_a, 0, sizeof(float) * N);
    memset(ws->time_buf_b, 0, sizeof(float) * N);

    int copy_len = MIN(num_frames, N);
    memcpy(ws->time_buf_a, signal_a, sizeof(float) * copy_len);
    memcpy(ws->time_buf_b, signal_b, sizeof(float) * copy_len);

    /*
     * Step 2: Forward FFT of both signals.
     */
    fftwf_execute(ws->plan_fwd_a);
    fftwf_execute(ws->plan_fwd_b);

    /*
     * Step 3: Compute cross-power spectrum with PHAT normalization.
     *
     * Cross-correlation R_ab(tau) = sum_t a(t) * b(t + tau)
     * In frequency domain: R_ab(f) = conj(A(f)) * B(f)
     *
     * PHAT weighting: R_phat(f) = R_ab(f) / |R_ab(f)|
     *
     * This retains only the phase information of the cross-spectrum,
     * producing the sharpest possible correlation peak.
     *
     * Sign convention: positive TDOA means signal arrives at mic B
     * LATER than mic A (i.e., mic A is closer to the source).
     */
    for (int k = 0; k < freq_size; k++) {
        float a_re = ws->freq_buf_a[k][0];
        float a_im = ws->freq_buf_a[k][1];
        float b_re = ws->freq_buf_b[k][0];
        float b_im = ws->freq_buf_b[k][1];

        /* Cross-spectrum: conj(X_a) * X_b */
        float cross_re = a_re * b_re + a_im * b_im;
        float cross_im = a_re * b_im - a_im * b_re;

        /* Magnitude of cross-spectrum */
        float mag = sqrtf(cross_re * cross_re + cross_im * cross_im) + PHAT_EPSILON;

        /* PHAT normalization */
        ws->cross_spectrum[k][0] = cross_re / mag;
        ws->cross_spectrum[k][1] = cross_im / mag;
    }

    /*
     * Step 4: Inverse FFT to get the cross-correlation function.
     * FFTW's c2r transform does not normalize, so the output is
     * scaled by N. We handle this when finding the peak.
     */
    fftwf_execute(ws->plan_inv);

    /*
     * Step 5: Find the peak of the correlation within the physically
     * possible delay range.
     *
     * For microphones separated by at most D meters, the maximum TDOA
     * is D/c seconds = D/c * sample_rate samples. We search within
     * this range to avoid picking up spurious peaks from noise.
     *
     * The correlation array layout (from FFTW's real-to-complex IFFT):
     *   Index 0:       zero lag
     *   Index 1..N/2:  positive lags (signal B is delayed)
     *   Index N/2..N-1: negative lags (signal A is delayed)
     *                   These correspond to lags -(N/2)..-1
     *
     * We compute the max physically possible separation across any
     * pair in our array. For a semicircular array of radius R,
     * the maximum distance is the diameter = 2*R.
     */
    float max_distance = 2.0f * ARRAY_RADIUS_M;
    float max_delay_sec = max_distance / SPEED_OF_SOUND;
    int max_delay_samples = (int)ceilf(max_delay_sec * (float)sample_rate) + 2;

    /* Clamp search range to buffer limits */
    if (max_delay_samples > N / 2) {
        max_delay_samples = N / 2;
    }

    float best_val = -1e30f;
    int best_idx = 0;

    /* Search positive lags: indices 0..max_delay_samples */
    for (int i = 0; i <= max_delay_samples; i++) {
        float val = ws->correlation[i];
        if (val > best_val) {
            best_val = val;
            best_idx = i;
        }
    }

    /* Search negative lags: indices (N - max_delay_samples)..N-1 */
    for (int i = N - max_delay_samples; i < N; i++) {
        float val = ws->correlation[i];
        if (val > best_val) {
            best_val = val;
            best_idx = i;
        }
    }

    /*
     * Step 6: Convert peak index to TDOA in seconds.
     * Apply parabolic interpolation for sub-sample precision.
     */
    float fractional_offset = parabolic_interpolation(ws->correlation, best_idx, N);

    /* Convert index to signed delay (negative lags are at the end of the array) */
    float delay_samples;
    if (best_idx <= N / 2) {
        delay_samples = (float)best_idx + fractional_offset;
    } else {
        delay_samples = (float)(best_idx - N) + fractional_offset;
    }

    *tdoa_out = delay_samples / (float)sample_rate;

    /* Normalize peak value by N (FFTW does not normalize IFFT) */
    *peak_out = best_val / (float)N;
}

void gcc_phat_compute_all_pairs(gcc_phat_workspace_t *ws,
                                 const float *multichannel,
                                 int num_frames,
                                 int sample_rate)
{
    if (!ws || !multichannel || num_frames <= 0) return;

    int ch = ws->num_channels;

    /*
     * Temporary buffers for extracting single-channel data from
     * the interleaved multichannel buffer.
     *
     * We allocate these on the stack if fft_size is reasonable (< 64K),
     * otherwise fall back to malloc. For 4096 * 4 bytes = 16 KB per
     * channel, stack allocation is fine.
     */
    float *mono_a = (float *)malloc(sizeof(float) * num_frames);
    float *mono_b = (float *)malloc(sizeof(float) * num_frames);

    if (!mono_a || !mono_b) {
        fprintf(stderr, "[gcc_phat] Failed to allocate mono extraction buffers\n");
        free(mono_a);
        free(mono_b);
        return;
    }

    for (int p = 0; p < ws->num_pairs; p++) {
        int mic_i = ws->pair_indices[p][0];
        int mic_j = ws->pair_indices[p][1];

        /* De-interleave: extract channel mic_i and mic_j */
        for (int f = 0; f < num_frames; f++) {
            mono_a[f] = multichannel[f * ch + mic_i];
            mono_b[f] = multichannel[f * ch + mic_j];
        }

        gcc_phat_compute_pair(ws, mono_a, mono_b, num_frames, sample_rate,
                               &ws->tdoa_results[p], &ws->peak_values[p]);
    }

    free(mono_a);
    free(mono_b);
}

int gcc_phat_get_pair_index(const gcc_phat_workspace_t *ws, int mic_i, int mic_j)
{
    if (!ws) return -1;

    /* Ensure i < j */
    if (mic_i > mic_j) {
        int tmp = mic_i;
        mic_i = mic_j;
        mic_j = tmp;
    }

    for (int p = 0; p < ws->num_pairs; p++) {
        if (ws->pair_indices[p][0] == mic_i && ws->pair_indices[p][1] == mic_j) {
            return p;
        }
    }

    return -1;  /* Not found */
}
