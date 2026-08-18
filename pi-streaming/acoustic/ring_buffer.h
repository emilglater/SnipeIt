/**
 * ring_buffer.h
 *
 * Thread-safe circular buffer for multichannel audio data.
 *
 * Why it works this way:
 *   The capture thread writes continuously at 48 kHz; the processing thread
 *   grabs a snapshot of recent audio when a trigger fires. One mutex covers
 *   both paths, held across the memcpys, so a snapshot never tears against a
 *   concurrent write.
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
    pthread_mutex_t mutex;          /* Protects write_pos, frames_written AND the
                                       data array -- both write and snapshot hold
                                       it across their memcpys. */
} ring_buffer_t;

/**
 * @brief Allocate and initialize a ring buffer.
 *
 * @param capacity_frames Number of frames the buffer holds.
 * @param num_channels    Number of audio channels per frame.
 *
 * @details The caller must eventually call ring_buffer_destroy() to free
 *          memory.
 *
 * @returns A pointer to the new ring buffer, or NULL on failure.
 */
ring_buffer_t *ring_buffer_create(int capacity_frames, int num_channels);

/**
 * @brief Free all memory associated with a ring buffer.
 *
 * @param rb Pointer to the ring buffer to destroy. May be NULL (no-op).
 */
void ring_buffer_destroy(ring_buffer_t *rb);

/**
 * @brief Write interleaved multichannel frames into the buffer.
 *
 * @param rb         The ring buffer.
 * @param frames     Pointer to interleaved float samples.
 *                   Layout: [f0_ch0, f0_ch1, ..., f1_ch0, f1_ch1, ...]
 * @param num_frames Number of frames to write.
 *
 * @details Wraps around automatically. If num_frames exceeds the buffer
 *          capacity, only the last (capacity) frames are retained.
 *          Thread-safe: acquires the internal mutex.
 */
void ring_buffer_write(ring_buffer_t *rb, const float *frames, int num_frames);

/**
 * @brief Copy the most recent N frames from the buffer.
 *
 * @param rb         The ring buffer.
 * @param output     Pre-allocated array to receive the data. Must hold at
 *                   least (num_frames * num_channels) floats.
 * @param num_frames Number of recent frames to copy.
 *
 * @details Thread-safe: acquires the internal mutex for the duration of the
 *          copy. The output is interleaved, matching the input format.
 *
 * @returns The number of frames actually copied, which may be less than
 *          requested if the buffer has not yet been filled that far.
 */
int ring_buffer_snapshot(ring_buffer_t *rb, float *output, int num_frames);

/**
 * @brief Return the total number of frames written since creation.
 *
 * @param rb The ring buffer.
 *
 * @details Useful for computing elapsed time. Thread-safe: reads under mutex.
 *
 * @returns Total frames written, or 0 if rb is NULL.
 */
int64_t ring_buffer_get_total_written(ring_buffer_t *rb);

#endif /* RING_BUFFER_H */
