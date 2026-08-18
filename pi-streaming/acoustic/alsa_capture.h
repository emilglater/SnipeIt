/**
 * alsa_capture.h
 *
 * Real-time multichannel audio capture from I2S INMP441 microphones
 * via the ALSA (Advanced Linux Sound Architecture) API.
 *
 * This module handles the low-level audio capture from the I2S
 * interface on the Raspberry Pi 5 and feeds the data into the
 * ring buffer for processing by the detection pipeline.
 *
 * The capture runs in its own high-priority thread so the buffer does not
 * overflow (an "xrun"). The RT priority is best-effort: it needs root or
 * CAP_SYS_NICE, and the thread runs at normal priority if that is refused.
 *
 * Integration:
 *   1. Create an alsa_capture instance.
 *   2. Set the ring buffer and onset detector references.
 *   3. Call alsa_capture_start() to begin capturing in background.
 *   4. The capture thread writes audio to the ring buffer and
 *      runs the onset detector on each chunk.
 *   5. When an onset is detected, the event_callback is called.
 *   6. Call alsa_capture_stop() to shut down cleanly.
 */

#ifndef ALSA_CAPTURE_H
#define ALSA_CAPTURE_H

#include <pthread.h>
#include "acoustic_config.h"
#include "ring_buffer.h"
#include "onset_detector.h"

/**
 * @brief Callback function type for onset detection events.
 *
 * @param timestamp_us Timestamp of the event in microseconds
 *                     (CLOCK_MONOTONIC).
 * @param user_data    Opaque pointer passed during setup.
 *
 * @details Invoked from the capture thread when the onset detector fires. The
 *          callback should be fast (e.g., set a flag or post to a queue) and
 *          must not block.
 */
typedef void (*onset_callback_t)(uint64_t timestamp_us, void *user_data);

/**
 * @brief ALSA capture state.
 */
typedef struct
{
    /* ALSA handle (void* to avoid including alsa/asoundlib.h in header) */
    void               *pcm_handle;

    /* Audio parameters */
    int                 sample_rate;
    int                 num_channels;
    int                 chunk_frames;        /* Frames per read (e.g., 480 = 10 ms) */

    /* Processing chain references */
    ring_buffer_t      *ring_buffer;
    onset_detector_t   *onset_detector;

    /* Event callback */
    onset_callback_t    onset_cb;
    void               *onset_cb_data;

    /* Capture thread */
    pthread_t           thread;
    volatile int        running;             /* 1 = running, 0 = stop requested */

    /* Statistics */
    unsigned long       total_frames_captured;
    unsigned long       xrun_count;          /* Failed reads: xruns plus any other
                                                snd_pcm_readi error */

    /* Device name */
    char                device_name[64];
} alsa_capture_t;

/**
 * @brief Allocate and configure an ALSA capture instance.
 *
 * @param device_name  ALSA device name (e.g., "hw:0", "plughw:0", "default").
 *                     Use "hw:0" for direct hardware access (lowest latency).
 *                     Use "plughw:0" for automatic format conversion.
 * @param sample_rate  Desired sample rate (e.g., 48000).
 * @param num_channels Number of channels to capture (2 for one I2S bus, 4 for
 *                     two buses, etc.).
 * @param chunk_frames Number of frames per read chunk. Smaller = lower latency
 *                     but higher CPU overhead. 480 frames at 48 kHz = 10 ms.
 *
 * @details Does NOT open the ALSA device or start capturing; call
 *          alsa_capture_start() for that.
 *
 * @returns A pointer to the capture instance, or NULL on failure.
 */
alsa_capture_t *alsa_capture_create(const char *device_name,
                                     int sample_rate,
                                     int num_channels,
                                     int chunk_frames);

/**
 * @brief Free all resources.
 *
 * @param cap The capture instance. May be NULL (no-op).
 *
 * @details If capture is running, stops it first.
 */
void alsa_capture_destroy(alsa_capture_t *cap);

/**
 * @brief Set the ring buffer for audio storage.
 *
 * @param cap The capture instance.
 * @param rb  The ring buffer to write captured audio into. Non-owning.
 *
 * @details Must be called before alsa_capture_start().
 */
void alsa_capture_set_ring_buffer(alsa_capture_t *cap, ring_buffer_t *rb);

/**
 * @brief Set the onset detector for event detection.
 *
 * @param cap The capture instance.
 * @param det The onset detector to run on each chunk. Non-owning.
 *
 * @details Must be called before alsa_capture_start().
 */
void alsa_capture_set_onset_detector(alsa_capture_t *cap, onset_detector_t *det);

/**
 * @brief Register a callback for onset events.
 *
 * @param cap       The capture instance.
 * @param callback  Function to call when an onset is detected.
 * @param user_data Opaque pointer passed to the callback.
 */
void alsa_capture_set_onset_callback(alsa_capture_t *cap,
                                      onset_callback_t callback,
                                      void *user_data);

/**
 * @brief Open the ALSA device and begin capturing.
 *
 * @param cap The capture instance.
 *
 * @details Spawns a background thread that continuously reads audio from the
 *          I2S interface, writes it to the ring buffer, runs the onset
 *          detector, and invokes the callback on events.
 *
 * @returns 0 on success, -1 on failure.
 */
int alsa_capture_start(alsa_capture_t *cap);

/**
 * @brief Stop capturing and close the ALSA device.
 *
 * @param cap The capture instance.
 *
 * @details Signals the capture thread to stop, joins it, and closes the ALSA
 *          handle. Safe to call multiple times. No-op when `running` is
 *          already 0 -- including when the capture thread cleared it by
 *          exiting on its own, in which case the handle is not closed here.
 */
void alsa_capture_stop(alsa_capture_t *cap);

/**
 * @brief Check if capture is active.
 *
 * @param cap The capture instance. May be NULL.
 *
 * @returns 1 if the capture thread is running, 0 otherwise (including NULL).
 */
int alsa_capture_is_running(const alsa_capture_t *cap);

/**
 * @brief Print available ALSA capture devices to stdout.
 *
 * @details Useful for debugging: helps identify the correct device name.
 */
void alsa_capture_list_devices(void);

#endif /* ALSA_CAPTURE_H */
