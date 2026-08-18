/**
 * ring_buffer.h
 *
 * Thread-safe circular buffer for multichannel audio data.
 *
 * Design rationale:
 *   The audio capture thread writes continuously at 48 kHz. The
 *   processing thread needs to grab a snapshot of recent audio when
 *   a trigger event occurs. This ring buffer supports both operations
 *   with minimal locking: a mutex protects the write pointer, and
 *   snapshot reads use a copy-under-lock strategy.
 *
 *   One "frame" = one sample from each channel (i.e., NUM_CHANNELS
 *   float values). The buffer stores RING_BUFFER_FRAMES frames.
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <pthread.h>
#include "acoustic_config.h"

typedef struct
{
    float       *data;              /* Flat array: [frame0_ch0, frame0_ch1, ..., frame1_ch0, ...] */
    int          capacity;          /* Total frames the buffer can hold */
    int          num_channels;      /* Channels per frame */
    int          write_pos;         /* Next frame index to write to (0..capacity-1) */
    int64_t      frames_written;    /* Total frames written since creation. Must stay
                                       64-bit: a 32-bit counter wraps after ~12.4 h at
                                       48 kHz, which drives snapshot() past its bounds. */
    pthread_mutex_t mutex;          /* Protects write_pos and frames_written */
} ring_buffer_t;

/**
 * ring_buffer_create - Allocate and initialize a ring buffer.
 *
 * @capacity_frames: Number of frames the buffer holds.
 * @num_channels:    Number of audio channels per frame.
 *
 * Returns a pointer to the new ring buffer, or NULL on failure.
 * The caller must eventually call ring_buffer_destroy() to free memory.
 */
ring_buffer_t *ring_buffer_create(int capacity_frames, int num_channels);

/**
 * ring_buffer_destroy - Free all memory associated with a ring buffer.
 *
 * @rb: Pointer to the ring buffer to destroy. May be NULL (no-op).
 */
void ring_buffer_destroy(ring_buffer_t *rb);

/**
 * ring_buffer_write - Write interleaved multichannel frames into the buffer.
 *
 * @rb:         The ring buffer.
 * @frames:     Pointer to interleaved float samples.
 *              Layout: [f0_ch0, f0_ch1, ..., f1_ch0, f1_ch1, ...]
 * @num_frames: Number of frames to write.
 *
 * This function wraps around automatically. If num_frames exceeds
 * the buffer capacity, only the last (capacity) frames are retained.
 *
 * Thread-safe: acquires the internal mutex.
 */
void ring_buffer_write(ring_buffer_t *rb, const float *frames, int num_frames);

/**
 * ring_buffer_snapshot - Copy the most recent N frames from the buffer.
 *
 * @rb:           The ring buffer.
 * @output:       Pre-allocated array to receive the data. Must hold at
 *                least (num_frames * num_channels) floats.
 * @num_frames:   Number of recent frames to copy.
 *
 * Returns the actual number of frames copied (may be less than requested
 * if the buffer has not yet been filled that far).
 *
 * Thread-safe: acquires the internal mutex for the duration of the copy.
 * The output is arranged identically to the input format (interleaved).
 */
int ring_buffer_snapshot(ring_buffer_t *rb, float *output, int num_frames);

/**
 * ring_buffer_get_total_written - Return the total number of frames
 * written since creation. Useful for computing elapsed time.
 *
 * Thread-safe: reads under mutex.
 */
int64_t ring_buffer_get_total_written(ring_buffer_t *rb);

#endif /* RING_BUFFER_H */
