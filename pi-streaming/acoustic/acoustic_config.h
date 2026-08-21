/**
 * acoustic_config.h
 *
 * Central configuration for the SnipeIt acoustic detection module.
 * Tunables live here so they can be changed without touching the
 * algorithm code.
 */

#ifndef ACOUSTIC_CONFIG_H
#define ACOUSTIC_CONFIG_H

#include <stdint.h>
#include <math.h>

/* ---------------------------------------------------------------------------
 * Audio Capture Parameters
 * ---------------------------------------------------------------------------
 * SAMPLE_RATE: Must match the I2S clock configuration.
 *   48000 Hz is the native rate for the INMP441, and fine enough in time to
 *   measure the delays across an array this size.
 *
 * NUM_CHANNELS: Number of microphones in the array.
 *   Start with 4 (2 I2S buses x 2 channels). Can be increased to 6
 *   by adding a third I2S bus.
 *
 * BITS_PER_SAMPLE: 32. The INMP441 outputs 24-bit data left-justified in a
 *   32-bit I2S frame, so alsa_capture reads S32_LE and divides by 2^31.
 * ------------------------------------------------------------------------ */
#define SAMPLE_RATE         48000
#define NUM_CHANNELS        4
#define BITS_PER_SAMPLE     32
#define BYTES_PER_SAMPLE    (BITS_PER_SAMPLE / 8)

/* ---------------------------------------------------------------------------
 * Ring Buffer Parameters
 * ---------------------------------------------------------------------------
 * RING_BUFFER_SECONDS: How much audio history to keep. 2 seconds is plenty
 *   to hold the audio from just before an event.
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
 * SNAPSHOT_FRAMES: How many frames the bridge pulls from the ring buffer for
 *   GCC-PHAT analysis. Must be <= FFT_SIZE. ring_buffer_snapshot() returns a
 *   TRAILING window -- the most recent N frames as of the call -- so this is
 *   not centred on the trigger. 4096 frames is 85 ms at 48 kHz.
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
 * SPEED_OF_SOUND: Value at 20 degrees Celsius. Compile-time only -- srp_phat
 *   and gcc_phat use this macro directly, so changing it for a different
 *   ambient temperature means editing here and rebuilding. The relation is
 *   c = 331 * sqrt(1 + T/273), T in Celsius.
 * ------------------------------------------------------------------------ */
#define AZIMUTH_MIN_DEG     (-90.0f)
#define AZIMUTH_MAX_DEG     (90.0f)
#define AZIMUTH_STEP_DEG    (1.0f)
#define SPEED_OF_SOUND      343.0f

/* ---------------------------------------------------------------------------
 * Microphone Array Geometry
 * ---------------------------------------------------------------------------
 * ARRAY_RADIUS_M: Radius of the semicircular array in meters.
 *   0.08 m (8 cm) trades angular resolution against spatial aliasing.
 *   gcc_phat derives its correlation search window from this, so it must
 *   match the MIC_POSITIONS table below.
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

/**
 * @brief One microphone's position in the array coordinate frame.
 */
typedef struct
{
    float x;  /* meters, positive = right */
    float y;  /* meters, positive = forward */
} mic_position_t;

/* Global microphone positions array (defined in acoustic_config.c) */
extern const mic_position_t MIC_POSITIONS[NUM_CHANNELS];

/* ---------------------------------------------------------------------------
 * Acoustic Event Structure
 * ---------------------------------------------------------------------------
 * The shape of one detection+localization result. Not currently populated:
 * acoustic_bridge_tick() serialises these same fields straight to JSON. Kept
 * as the definition to use if an event ever needs queueing or logging.
 * ------------------------------------------------------------------------ */
typedef struct
{
    uint64_t    timestamp_us;       /* CLOCK_MONOTONIC microseconds. NOT wall clock --
                                       the origin is arbitrary (boot on Linux), so only
                                       differences between events are meaningful. */
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

/* Both evaluate their arguments twice -- never pass an expression with side
 * effects. Defined unconditionally, so this header cannot be combined with one
 * that also defines MIN/MAX (sys/param.h, for instance). */
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

#endif /* ACOUSTIC_CONFIG_H */
