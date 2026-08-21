/**
 * acoustic_bridge.c
 *
 * Adapter that wires the SnipeIt acoustic detection pipeline into the
 * pi-streaming binary's WebSocketServer.
 *
 * Pattern mirrors ddl_bridge.c:
 *   - acoustic_bridge_start(ws) brings up the capture pipeline
 *   - acoustic_bridge_tick(b)   drains pending events (called from main loop)
 *   - acoustic_bridge_stop(b)   tears everything down
 *
 * When an onset fires (from the ALSA capture thread), a flag is set; the next
 * tick on the main thread snapshots audio, runs GCC-PHAT + SRP-PHAT, builds
 * the event JSON, and calls ws_send_json() to push it to the Android app.
 */

#include "acoustic_bridge.h"
#include "websocket_server.h"

#include "acoustic_config.h"
#include "ring_buffer.h"
#include "onset_detector.h"
#include "gcc_phat.h"
#include "srp_phat.h"
#include "alsa_capture.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AcousticBridge
{
    WebSocketServer*       ws;            /* Non-owning */
    ring_buffer_t*         rb;
    onset_detector_t*      det;
    gcc_phat_workspace_t*  gcc_ws;
    alsa_capture_t*        cap;

    /* Set from the capture thread, read+cleared from the main thread tick. */
    volatile int           event_pending;
    volatile uint64_t      event_timestamp;
};

/* Capture-thread callback. Keep this minimal — just set the flag. */
static void on_onset_detected(uint64_t timestamp_us, void* user_data)
{
    AcousticBridge* b = (AcousticBridge*)user_data;
    if (b == NULL) return;
    b->event_timestamp = timestamp_us;
    b->event_pending   = 1;
}

AcousticBridge* acoustic_bridge_start(WebSocketServer* ws)
{
    if (ws == NULL)
    {
        return NULL;
    }

    AcousticBridge* b = calloc(1, sizeof(*b));
    if (b == NULL)
    {
        return NULL;
    }

    b->ws = ws;

    b->rb = ring_buffer_create(RING_BUFFER_FRAMES, NUM_CHANNELS);
    if (b->rb == NULL)
    {
        fprintf(stderr, "[ACOUSTIC] ring_buffer_create failed\n");
        goto fail;
    }

    b->det = onset_detector_create(SAMPLE_RATE, HIGHPASS_CUTOFF_HZ,
                                    ONSET_THRESHOLD, REFRACTORY_MS);
    if (b->det == NULL)
    {
        fprintf(stderr, "[ACOUSTIC] onset_detector_create failed\n");
        goto fail;
    }

    b->gcc_ws = gcc_phat_create(FFT_SIZE, NUM_CHANNELS);
    if (b->gcc_ws == NULL)
    {
        fprintf(stderr, "[ACOUSTIC] gcc_phat_create failed\n");
        goto fail;
    }

    /* 10 ms chunks at the configured sample rate */
    const int chunk_frames = SAMPLE_RATE / 100;
    b->cap = alsa_capture_create("plughw:0", SAMPLE_RATE, NUM_CHANNELS, chunk_frames);
    if (b->cap == NULL)
    {
        fprintf(stderr, "[ACOUSTIC] alsa_capture_create failed (is plughw:0 available?)\n");
        goto fail;
    }

    alsa_capture_set_ring_buffer(b->cap, b->rb);
    alsa_capture_set_onset_detector(b->cap, b->det);
    alsa_capture_set_onset_callback(b->cap, on_onset_detected, b);

    if (alsa_capture_start(b->cap) != 0)
    {
        fprintf(stderr, "[ACOUSTIC] alsa_capture_start failed\n");
        goto fail;
    }

    printf("[ACOUSTIC] Started — %d-mic array, %d Hz, threshold %.1f\n",
           NUM_CHANNELS, SAMPLE_RATE, (double)ONSET_THRESHOLD);
    return b;

fail:
    acoustic_bridge_stop(b);
    return NULL;
}

int acoustic_bridge_tick(AcousticBridge* b)
{
    if (b == NULL || !b->event_pending)
    {
        return 0;
    }

    /* Claim the pending event. The flag is set on the capture thread and
     * read+cleared here on the main thread. */
    b->event_pending = 0;
    const uint64_t timestamp_us = b->event_timestamp;

    /* Short-circuit when nobody is listening — saves an FFT + grid search */
    if (!ws_is_client_connected(b->ws))
    {
        return 0;
    }

    /* Snapshot audio from the ring buffer for analysis */
    float* snapshot = (float*)malloc(sizeof(float) * SNAPSHOT_FRAMES * NUM_CHANNELS);
    if (snapshot == NULL)
    {
        fprintf(stderr, "[ACOUSTIC] snapshot malloc failed\n");
        return 0;
    }

    /* Trailing window: this returns the most recent SNAPSHOT_FRAMES frames as
     * of right now on the main thread, not a window centred on the onset. That
     * is 85 ms at 48 kHz, and the onset fired on the capture thread one tick
     * (~20 ms) earlier -- see the main loop pacing note in src/main.c. */
    int frames_got = ring_buffer_snapshot(b->rb, snapshot, SNAPSHOT_FRAMES);
    if (frames_got < SNAPSHOT_FRAMES / 2)
    {
        free(snapshot);
        return 0;
    }

    gcc_phat_compute_all_pairs(b->gcc_ws, snapshot, frames_got, SAMPLE_RATE);

    srp_phat_result_t srp_result;
    srp_phat_estimate(b->gcc_ws, snapshot, frames_got, SAMPLE_RATE, &srp_result);

    /* Peak amplitude, plus a crude duration: this counts every sample over
     * amp_thresh across all channels and divides by the channel count, so it is
     * the TOTAL time spent above threshold, not a contiguous event length.
     * Three spaced shots and one long reverberant tail give the same number. */
    float peak_amp = 0.0f;
    int   above_thresh = 0;
    const float amp_thresh = 0.05f;
    for (int i = 0; i < frames_got * NUM_CHANNELS; i++)
    {
        const float a = fabsf(snapshot[i]);
        if (a > peak_amp) peak_amp = a;
        if (a > amp_thresh) above_thresh++;
    }
    const float duration_ms = ((float)above_thresh / (float)NUM_CHANNELS)
                              / (float)SAMPLE_RATE * 1000.0f;

    const int valid = (srp_result.confidence > 0.3f && peak_amp > 0.05f) ? 1 : 0;

    /* Build event JSON. The Android app parses these key names directly -- do
     * not rename or reorder fields without a matching change on the app side. */
    char json[512];
    const int n = snprintf(json, sizeof(json),
        "{"
            "\"type\":\"acoustic_event\","
            "\"timestamp_us\":%llu,"
            "\"azimuth_deg\":%.1f,"
            "\"confidence\":%.2f,"
            "\"peak_amplitude\":%.4f,"
            "\"duration_ms\":%.1f,"
            "\"valid\":%s"
        "}",
        (unsigned long long)timestamp_us,
        (double)srp_result.azimuth_deg,
        (double)srp_result.confidence,
        (double)peak_amp,
        (double)duration_ms,
        valid ? "true" : "false");

    if (n > 0 && (size_t)n < sizeof(json))
    {
        printf("[EVENT] %s\n", json);
        fflush(stdout);
        if (ws_send_json(b->ws, json, (size_t)n) != 0)
        {
            fprintf(stderr, "[ACOUSTIC] ws_send_json failed (queue full?)\n");
        }
    }

    free(snapshot);
    return 1;
}

void acoustic_bridge_stop(AcousticBridge* b)
{
    if (b == NULL) return;

    if (b->cap != NULL)
    {
        alsa_capture_stop(b->cap);
        alsa_capture_destroy(b->cap);
    }
    if (b->gcc_ws != NULL)
    {
        gcc_phat_destroy(b->gcc_ws);
    }
    if (b->det != NULL)
    {
        onset_detector_destroy(b->det);
    }
    if (b->rb != NULL)
    {
        ring_buffer_destroy(b->rb);
    }
    free(b);
}
