/**
 * acoustic_config.h
 *
 * Central configuration for the SnipeIt Acoustic Detection Module.
 * All tunable parameters are defined here so they can be adjusted
 * without modifying the algorithmic code.
 *
 * Author: SnipeIt Team
 * Date: May 2026
 */

#ifndef ACOUSTIC_CONFIG_H
#define ACOUSTIC_CONFIG_H

#include <stdint.h>
#include <math.h>

/* ---------------------------------------------------------------------------
 * Audio Capture Parameters
 * ---------------------------------------------------------------------------
 * SAMPLE_RATE: Must match the I2S clock configuration.
 *   48000 Hz is the native rate for INMP441 and provides sufficient
 *   time resolution for TDOA at our array dimensions.
 *
 * NUM_CHANNELS: Number of microphones in the array.
 *   Start with 4 (2 I2S buses x 2 channels). Can be increased to 6
 *   by adding a third I2S bus.
 *
 * SAMPLE_FORMAT: 32-bit signed integer. INMP441 outputs 24-bit data
 *   packed into 32-bit I2S frames. We read as int32 and normalize.
 * ------------------------------------------------------------------------ */
#define SAMPLE_RATE         48000
#define NUM_CHANNELS        4
#define BITS_PER_SAMPLE     32
#define BYTES_PER_SAMPLE    (BITS_PER_SAMPLE / 8)

/* ---------------------------------------------------------------------------
 * Ring Buffer Parameters
 * ---------------------------------------------------------------------------
 * RING_BUFFER_SECONDS: How much audio history to keep. 2 seconds gives
 *   ample margin for capturing the pre-trigger portion of an event.
 *
 * RING_BUFFER_FRAMES: Total frames in the ring buffer. One "frame"
 *   contains one sample from each channel.
 * ------------------------------------------------------------------------ */
#define RING_BUFFER_SECONDS 2
#define RING_BUFFER_FRAMES  (SAMPLE_RATE * RING_BUFFER_SECONDS)

/* ---------------------------------------------------------------------------
 * Onset Detector Parameters
 * ---------------------------------------------------------------------------
 * SHORT_WINDOW_MS: Duration of the short-term energy window.
 * LONG_WINDOW_MS: Duration of the long-term energy window.
 * ONSET_THRESHOLD: Ratio of short/long energy that triggers detection.
 *   Higher = fewer false positives but may miss quieter events.
 * REFRACTORY_MS: Minimum time between successive triggers, to avoid
 *   re-triggering on echoes or the tail of the same event.
 * HIGHPASS_CUTOFF_HZ: Cutoff frequency for the wind-noise rejection
 *   high-pass filter. 300 Hz rejects most wind noise while preserving
 *   the lower frequencies of muzzle blast signals (200-500 Hz).
 * ------------------------------------------------------------------------ */
#define SHORT_WINDOW_MS     10
#define LONG_WINDOW_MS      500
#define ONSET_THRESHOLD     10.0f
#define REFRACTORY_MS       500
#define HIGHPASS_CUTOFF_HZ  300.0f

/* ---------------------------------------------------------------------------
 * GCC-PHAT Parameters
 * ---------------------------------------------------------------------------
 * FFT_SIZE: Length of the FFT used in cross-correlation. Must be a
 *   power of 2. 4096 at 48 kHz gives ~85 ms analysis window, which
 *   is more than enough for impulsive events (typically < 5 ms).
 *
 * SNAPSHOT_FRAMES: Number of frames to capture around a trigger event
 *   for GCC-PHAT analysis. Should be <= FFT_SIZE.
 *
 * PHAT_EPSILON: Small value added to the denominator of the PHAT
 *   normalization to avoid division by zero in silent regions.
 *
 * MAX_MIC_PAIRS: Maximum number of unique microphone pairs.
 *   For N mics, this is N*(N-1)/2.
 * ------------------------------------------------------------------------ */
#define FFT_SIZE            4096
#define SNAPSHOT_FRAMES     4096
#define PHAT_EPSILON        1e-10f
#define MAX_MIC_PAIRS       (NUM_CHANNELS * (NUM_CHANNELS - 1) / 2)

/* ---------------------------------------------------------------------------
 * SRP-PHAT Parameters
 * ---------------------------------------------------------------------------
 * AZIMUTH_MIN_DEG / AZIMUTH_MAX_DEG: The angular search range.
 *   For 180-degree forward coverage: -90 to +90 degrees, where
 *   0 degrees is the forward direction (camera boresight).
 *
 * AZIMUTH_STEP_DEG: Angular resolution of the search grid.
 *   1 degree gives 181 candidate angles, which is fast enough for
 *   real-time on a Pi 5.
 *
 * SPEED_OF_SOUND: Default value at 20 degrees Celsius.
 *   For temperature-compensated operation, recalculate using:
 *   c = 331 * sqrt(1 + T/273) where T is in Celsius.
 * ------------------------------------------------------------------------ */
#define AZIMUTH_MIN_DEG     (-90.0f)
#define AZIMUTH_MAX_DEG     (90.0f)
#define AZIMUTH_STEP_DEG    (1.0f)
#define SPEED_OF_SOUND      343.0f

/* ---------------------------------------------------------------------------
 * Microphone Array Geometry
 * ---------------------------------------------------------------------------
 * ARRAY_RADIUS_M: Radius of the semicircular array in meters.
 *   0.08 m (8 cm) provides a good balance between angular resolution
 *   and spatial aliasing avoidance.
 *
 * MIC_POSITIONS: Defined in acoustic_config.c as a global array.
 *   Coordinates are in meters, with the array center at the origin.
 *   +X is to the right, +Y is forward (camera boresight direction).
 *
 *   For 4 mics on a semicircle covering -90 to +90 degrees:
 *     Mic 0: -90 deg  -> (-R,  0)
 *     Mic 1: -30 deg  -> (-R*sin(30), R*cos(30))
 *     Mic 2: +30 deg  -> (+R*sin(30), R*cos(30))
 *     Mic 3: +90 deg  -> (+R,  0)
 * ------------------------------------------------------------------------ */
#define ARRAY_RADIUS_M      0.08f

/* Microphone position structure */
typedef struct {
    float x;  /* meters, positive = right */
    float y;  /* meters, positive = forward */
} mic_position_t;

/* Global microphone positions array (defined in acoustic_config.c) */
extern const mic_position_t MIC_POSITIONS[NUM_CHANNELS];

/* ---------------------------------------------------------------------------
 * Acoustic Event Structure
 * ---------------------------------------------------------------------------
 * This is the output of the detection + localization pipeline, passed
 * to the WebSocket reporter for transmission to the Android app.
 * ------------------------------------------------------------------------ */
typedef struct {
    uint64_t    timestamp_us;       /* Microseconds since epoch (CLOCK_MONOTONIC) */
    float       azimuth_deg;        /* Estimated bearing: -90 to +90 degrees */
    float       confidence;         /* 0.0 to 1.0, based on SRP-PHAT peak quality */
    float       peak_amplitude;     /* Normalized peak amplitude of the event */
    float       duration_ms;        /* Estimated duration of the impulsive event */
    int         valid;              /* 1 if the event passed all quality checks */
} acoustic_event_t;

/* ---------------------------------------------------------------------------
 * Utility Macros
 * ------------------------------------------------------------------------ */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG_TO_RAD(d)   ((float)(d) * (float)M_PI / 180.0f)
#define RAD_TO_DEG(r)   ((float)(r) * 180.0f / (float)M_PI)

/* Minimum of two values */
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
/* Maximum of two values */
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

#endif /* ACOUSTIC_CONFIG_H */
