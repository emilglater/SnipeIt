/**
 * alsa_capture.c
 *
 * Real-time ALSA audio capture implementation.
 *
 * The INMP441 outputs 24-bit audio in a 32-bit I2S frame. The ALSA
 * driver reads this as SND_PCM_FORMAT_S32_LE (32-bit signed little-
 * endian). We normalize to float [-1.0, 1.0] by dividing by 2^31.
 *
 * The capture thread runs with SCHED_FIFO real-time priority (if
 * available) to minimize the chance of xruns (buffer overruns).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include "alsa_capture.h"

/* ---------------------------------------------------------------------------
 * Internal: Get current timestamp in microseconds (CLOCK_MONOTONIC)
 * ------------------------------------------------------------------------ */
static uint64_t get_timestamp_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ---------------------------------------------------------------------------
 * Internal: Convert interleaved int32 samples to interleaved float.
 *
 * INMP441 outputs 24-bit data left-justified in a 32-bit word.
 * The lower 8 bits are zero. We treat the full 32-bit value as a
 * signed integer and normalize to [-1.0, 1.0].
 * ------------------------------------------------------------------------ */
static void convert_s32_to_float(const int32_t *input, float *output,
                                  int num_frames, int num_channels)
{
    int total_samples = num_frames * num_channels;
    float scale = 1.0f / 2147483648.0f;  /* 1 / 2^31 */

    for (int i = 0; i < total_samples; i++) {
        output[i] = (float)input[i] * scale;
    }
}

/* ---------------------------------------------------------------------------
 * Internal: Downmix multichannel to mono (average of all channels).
 * Used by the onset detector which operates on a single channel.
 * ------------------------------------------------------------------------ */
static void downmix_to_mono(const float *multichannel, float *mono,
                             int num_frames, int num_channels)
{
    float scale = 1.0f / (float)num_channels;
    for (int f = 0; f < num_frames; f++) {
        float sum = 0.0f;
        for (int ch = 0; ch < num_channels; ch++) {
            sum += multichannel[f * num_channels + ch];
        }
        mono[f] = sum * scale;
    }
}

/* ---------------------------------------------------------------------------
 * Internal: ALSA xrun recovery.
 *
 * An xrun occurs when the capture buffer overflows because the
 * application did not read data fast enough. We recover by calling
 * snd_pcm_prepare() to reset the stream.
 * ------------------------------------------------------------------------ */
static int xrun_recovery(snd_pcm_t *handle, int err)
{
    if (err == -EPIPE) {
        /* Buffer overrun */
        fprintf(stderr, "[alsa_capture] XRUN (buffer overrun), recovering...\n");
        err = snd_pcm_prepare(handle);
        if (err < 0) {
            fprintf(stderr, "[alsa_capture] Cannot recover from XRUN: %s\n",
                    snd_strerror(err));
        }
    } else if (err == -ESTRPIPE) {
        /* Suspended (e.g., by power management) */
        fprintf(stderr, "[alsa_capture] Stream suspended, recovering...\n");
        while ((err = snd_pcm_resume(handle)) == -EAGAIN) {
            usleep(100000);  /* Wait for resume to complete */
        }
        if (err < 0) {
            err = snd_pcm_prepare(handle);
        }
    }
    return err;
}

/* ---------------------------------------------------------------------------
 * Internal: Capture thread main function.
 * ------------------------------------------------------------------------ */
static void *capture_thread_func(void *arg)
{
    alsa_capture_t *cap = (alsa_capture_t *)arg;
    snd_pcm_t *handle = (snd_pcm_t *)cap->pcm_handle;

    int chunk = cap->chunk_frames;
    int ch    = cap->num_channels;

    /* Allocate buffers for one chunk */
    int32_t *raw_buf   = (int32_t *)malloc(sizeof(int32_t) * chunk * ch);
    float   *float_buf = (float *)malloc(sizeof(float) * chunk * ch);
    float   *mono_buf  = (float *)malloc(sizeof(float) * chunk);

    if (!raw_buf || !float_buf || !mono_buf) {
        fprintf(stderr, "[alsa_capture] Failed to allocate capture buffers\n");
        free(raw_buf);
        free(float_buf);
        free(mono_buf);
        cap->running = 0;
        return NULL;
    }

    fprintf(stdout, "[alsa_capture] Capture thread started (%d Hz, %d ch, %d frames/chunk)\n",
            cap->sample_rate, ch, chunk);

    while (cap->running) {
        /* Read one chunk of interleaved audio */
        int frames_read = snd_pcm_readi(handle, raw_buf, chunk);

        if (frames_read < 0) {
            /* Handle xrun or other error */
            frames_read = xrun_recovery(handle, frames_read);
            if (frames_read < 0) {
                fprintf(stderr, "[alsa_capture] Read error: %s\n",
                        snd_strerror(frames_read));
                cap->xrun_count++;
                continue;
            }
            cap->xrun_count++;
            continue;
        }

        if (frames_read == 0) continue;

        /* Convert int32 -> float */
        convert_s32_to_float(raw_buf, float_buf, frames_read, ch);

        /* Write to ring buffer */
        if (cap->ring_buffer) {
            ring_buffer_write(cap->ring_buffer, float_buf, frames_read);
        }

        /* Run onset detector on mono downmix */
        if (cap->onset_detector) {
            downmix_to_mono(float_buf, mono_buf, frames_read, ch);

            if (onset_detector_process(cap->onset_detector, mono_buf, frames_read)) {
                uint64_t ts = get_timestamp_us();
                fprintf(stdout, "[alsa_capture] Onset detected at %llu us\n",
                        (unsigned long long)ts);

                if (cap->onset_cb) {
                    cap->onset_cb(ts, cap->onset_cb_data);
                }
            }
        }

        cap->total_frames_captured += (unsigned long)frames_read;
    }

    free(raw_buf);
    free(float_buf);
    free(mono_buf);

    fprintf(stdout, "[alsa_capture] Capture thread stopped. Total frames: %lu, Xruns: %lu\n",
            cap->total_frames_captured, cap->xrun_count);

    return NULL;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

alsa_capture_t *alsa_capture_create(const char *device_name,
                                     int sample_rate,
                                     int num_channels,
                                     int chunk_frames)
{
    alsa_capture_t *cap = (alsa_capture_t *)calloc(1, sizeof(alsa_capture_t));
    if (!cap) {
        fprintf(stderr, "[alsa_capture] Failed to allocate capture instance\n");
        return NULL;
    }

    strncpy(cap->device_name, device_name, sizeof(cap->device_name) - 1);
    cap->sample_rate  = sample_rate;
    cap->num_channels = num_channels;
    cap->chunk_frames = chunk_frames;
    cap->running      = 0;
    cap->pcm_handle   = NULL;

    return cap;
}

void alsa_capture_destroy(alsa_capture_t *cap)
{
    if (!cap) return;

    if (cap->running) {
        alsa_capture_stop(cap);
    }

    free(cap);
}

void alsa_capture_set_ring_buffer(alsa_capture_t *cap, ring_buffer_t *rb)
{
    if (cap) cap->ring_buffer = rb;
}

void alsa_capture_set_onset_detector(alsa_capture_t *cap, onset_detector_t *det)
{
    if (cap) cap->onset_detector = det;
}

void alsa_capture_set_onset_callback(alsa_capture_t *cap,
                                      onset_callback_t callback,
                                      void *user_data)
{
    if (!cap) return;
    cap->onset_cb      = callback;
    cap->onset_cb_data = user_data;
}

int alsa_capture_start(alsa_capture_t *cap)
{
    if (!cap) return -1;
    if (cap->running) {
        fprintf(stderr, "[alsa_capture] Already running\n");
        return -1;
    }

    /* Open the ALSA capture device */
    snd_pcm_t *handle;
    int err = snd_pcm_open(&handle, cap->device_name, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "[alsa_capture] Cannot open device '%s': %s\n",
                cap->device_name, snd_strerror(err));
        fprintf(stderr, "[alsa_capture] Hint: Run 'arecord -l' to list available devices.\n");
        return -1;
    }
    cap->pcm_handle = handle;

    /* Configure hardware parameters */
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(handle, hw_params);

    /* Set interleaved access mode */
    err = snd_pcm_hw_params_set_access(handle, hw_params,
                                        SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "[alsa_capture] Cannot set access mode: %s\n", snd_strerror(err));
        snd_pcm_close(handle);
        cap->pcm_handle = NULL;
        return -1;
    }

    /* Set sample format: 32-bit signed (INMP441 outputs 24-bit in 32-bit frame) */
    err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S32_LE);
    if (err < 0) {
        fprintf(stderr, "[alsa_capture] Cannot set format S32_LE: %s\n", snd_strerror(err));
        fprintf(stderr, "[alsa_capture] Trying S24_LE...\n");
        err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S24_LE);
        if (err < 0) {
            fprintf(stderr, "[alsa_capture] Cannot set format S24_LE: %s\n", snd_strerror(err));
            snd_pcm_close(handle);
            cap->pcm_handle = NULL;
            return -1;
        }
    }

    /* Set number of channels */
    err = snd_pcm_hw_params_set_channels(handle, hw_params, (unsigned int)cap->num_channels);
    if (err < 0) {
        fprintf(stderr, "[alsa_capture] Cannot set %d channels: %s\n",
                cap->num_channels, snd_strerror(err));
        snd_pcm_close(handle);
        cap->pcm_handle = NULL;
        return -1;
    }

    /* Set sample rate */
    unsigned int rate = (unsigned int)cap->sample_rate;
    err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, 0);
    if (err < 0) {
        fprintf(stderr, "[alsa_capture] Cannot set rate %d: %s\n",
                cap->sample_rate, snd_strerror(err));
        snd_pcm_close(handle);
        cap->pcm_handle = NULL;
        return -1;
    }
    if ((int)rate != cap->sample_rate) {
        fprintf(stderr, "[alsa_capture] Rate adjusted from %d to %u\n",
                cap->sample_rate, rate);
        cap->sample_rate = (int)rate;
    }

    /* Set buffer size (larger = more latency but fewer xruns) */
    snd_pcm_uframes_t buffer_size = (snd_pcm_uframes_t)(cap->sample_rate / 4);  /* 250 ms */
    err = snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &buffer_size);
    if (err < 0) {
        fprintf(stderr, "[alsa_capture] Cannot set buffer size: %s\n", snd_strerror(err));
    }

    /* Set period size (frames per hardware interrupt) */
    snd_pcm_uframes_t period_size = (snd_pcm_uframes_t)cap->chunk_frames;
    err = snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period_size, 0);
    if (err < 0) {
        fprintf(stderr, "[alsa_capture] Cannot set period size: %s\n", snd_strerror(err));
    }

    /* Apply hardware parameters */
    err = snd_pcm_hw_params(handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "[alsa_capture] Cannot apply hw params: %s\n", snd_strerror(err));
        snd_pcm_close(handle);
        cap->pcm_handle = NULL;
        return -1;
    }

    /* Print actual configuration */
    snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size);
    snd_pcm_hw_params_get_period_size(hw_params, &period_size, 0);
    fprintf(stdout, "[alsa_capture] Opened '%s': %u Hz, %d ch, buffer=%lu, period=%lu\n",
            cap->device_name, rate, cap->num_channels,
            (unsigned long)buffer_size, (unsigned long)period_size);

    /* Prepare the device */
    err = snd_pcm_prepare(handle);
    if (err < 0) {
        fprintf(stderr, "[alsa_capture] Cannot prepare device: %s\n", snd_strerror(err));
        snd_pcm_close(handle);
        cap->pcm_handle = NULL;
        return -1;
    }

    /* Start capture thread */
    cap->running = 1;
    err = pthread_create(&cap->thread, NULL, capture_thread_func, cap);
    if (err != 0) {
        fprintf(stderr, "[alsa_capture] Cannot create capture thread: %s\n", strerror(err));
        cap->running = 0;
        snd_pcm_close(handle);
        cap->pcm_handle = NULL;
        return -1;
    }

    /*
     * Optionally set real-time scheduling for the capture thread.
     * This reduces the chance of xruns under load. Requires root
     * or appropriate capabilities (CAP_SYS_NICE).
     */
    struct sched_param sched;
    sched.sched_priority = 80;
    if (pthread_setschedparam(cap->thread, SCHED_FIFO, &sched) != 0) {
        fprintf(stderr, "[alsa_capture] Note: Could not set RT priority "
                "(run as root or set CAP_SYS_NICE for lower latency)\n");
    }

    return 0;
}

void alsa_capture_stop(alsa_capture_t *cap)
{
    if (!cap || !cap->running) return;

    cap->running = 0;
    pthread_join(cap->thread, NULL);

    if (cap->pcm_handle) {
        snd_pcm_drop((snd_pcm_t *)cap->pcm_handle);
        snd_pcm_close((snd_pcm_t *)cap->pcm_handle);
        cap->pcm_handle = NULL;
    }
}

int alsa_capture_is_running(const alsa_capture_t *cap)
{
    return cap ? cap->running : 0;
}

void alsa_capture_list_devices(void)
{
    int card = -1;
    fprintf(stdout, "\n[alsa_capture] Available capture devices:\n");

    while (snd_card_next(&card) >= 0 && card >= 0) {
        char *name = NULL;
        snd_card_get_name(card, &name);
        fprintf(stdout, "  Card %d: %s\n", card, name ? name : "unknown");
        free(name);

        /* List devices on this card */
        snd_ctl_t *ctl;
        char card_id[32];
        snprintf(card_id, sizeof(card_id), "hw:%d", card);

        if (snd_ctl_open(&ctl, card_id, 0) >= 0) {
            int dev = -1;
            while (snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0) {
                fprintf(stdout, "    Device hw:%d,%d\n", card, dev);
            }
            snd_ctl_close(ctl);
        }
    }
    fprintf(stdout, "\n");
}
