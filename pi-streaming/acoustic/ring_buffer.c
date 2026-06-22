/**
 * ring_buffer.c
 *
 * Thread-safe circular buffer for multichannel audio.
 *
 * Memory layout: a flat array of (capacity * num_channels) floats.
 * Frame i, channel c is at index: (i * num_channels + c).
 * The write_pos wraps around modulo capacity.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "ring_buffer.h"

ring_buffer_t *ring_buffer_create(int capacity_frames, int num_channels)
{
    if (capacity_frames <= 0 || num_channels <= 0) {
        fprintf(stderr, "[ring_buffer] Invalid parameters: capacity=%d, channels=%d\n",
                capacity_frames, num_channels);
        return NULL;
    }

    ring_buffer_t *rb = (ring_buffer_t *)calloc(1, sizeof(ring_buffer_t));
    if (!rb) {
        fprintf(stderr, "[ring_buffer] Failed to allocate ring_buffer_t\n");
        return NULL;
    }

    size_t data_size = (size_t)capacity_frames * (size_t)num_channels * sizeof(float);
    rb->data = (float *)calloc(1, data_size);
    if (!rb->data) {
        fprintf(stderr, "[ring_buffer] Failed to allocate %zu bytes for audio data\n", data_size);
        free(rb);
        return NULL;
    }

    rb->capacity       = capacity_frames;
    rb->num_channels   = num_channels;
    rb->write_pos      = 0;
    rb->frames_written = 0;

    if (pthread_mutex_init(&rb->mutex, NULL) != 0) {
        fprintf(stderr, "[ring_buffer] Failed to initialize mutex\n");
        free(rb->data);
        free(rb);
        return NULL;
    }

    return rb;
}

void ring_buffer_destroy(ring_buffer_t *rb)
{
    if (!rb) return;

    pthread_mutex_destroy(&rb->mutex);
    free(rb->data);
    free(rb);
}

void ring_buffer_write(ring_buffer_t *rb, const float *frames, int num_frames)
{
    if (!rb || !frames || num_frames <= 0) return;

    pthread_mutex_lock(&rb->mutex);

    int ch = rb->num_channels;

    /*
     * If the incoming data is larger than the buffer, skip to the
     * last (capacity) frames -- earlier frames would be overwritten anyway.
     */
    if (num_frames > rb->capacity) {
        int skip = num_frames - rb->capacity;
        frames     += skip * ch;
        num_frames  = rb->capacity;
    }

    /*
     * Copy in up to two segments:
     *   Segment 1: from write_pos to end of buffer (or fewer if data fits).
     *   Segment 2: wraparound from the beginning (if needed).
     */
    int first_chunk = MIN(num_frames, rb->capacity - rb->write_pos);
    memcpy(&rb->data[rb->write_pos * ch], frames, (size_t)first_chunk * ch * sizeof(float));

    int remaining = num_frames - first_chunk;
    if (remaining > 0) {
        memcpy(&rb->data[0], &frames[first_chunk * ch], (size_t)remaining * ch * sizeof(float));
    }

    rb->write_pos       = (rb->write_pos + num_frames) % rb->capacity;
    rb->frames_written += num_frames;

    pthread_mutex_unlock(&rb->mutex);
}

int ring_buffer_snapshot(ring_buffer_t *rb, float *output, int num_frames)
{
    if (!rb || !output || num_frames <= 0) return 0;

    pthread_mutex_lock(&rb->mutex);

    int ch = rb->num_channels;

    /* Clamp to the amount of data actually available */
    int available = MIN(rb->frames_written, rb->capacity);
    if (num_frames > available) {
        num_frames = available;
    }

    /*
     * The most recent frame is at (write_pos - 1) mod capacity.
     * The oldest frame we want is (write_pos - num_frames) mod capacity.
     * We call this the "read start" position.
     */
    int read_start = (rb->write_pos - num_frames + rb->capacity) % rb->capacity;

    /*
     * Copy in up to two segments (similar logic to write, but reading).
     */
    int first_chunk = MIN(num_frames, rb->capacity - read_start);
    memcpy(output, &rb->data[read_start * ch], (size_t)first_chunk * ch * sizeof(float));

    int remaining = num_frames - first_chunk;
    if (remaining > 0) {
        memcpy(&output[first_chunk * ch], &rb->data[0], (size_t)remaining * ch * sizeof(float));
    }

    pthread_mutex_unlock(&rb->mutex);

    return num_frames;
}

int ring_buffer_get_total_written(ring_buffer_t *rb)
{
    if (!rb) return 0;

    pthread_mutex_lock(&rb->mutex);
    int total = rb->frames_written;
    pthread_mutex_unlock(&rb->mutex);

    return total;
}
