/**
 * pose_ring.c
 *
 * Implementation of the frame_id -> capture-time pose ring buffer.
 * See pose_ring.h for the design rationale.
 */

#include "pose_ring.h"

#include <stdlib.h>

PoseRing *pose_ring_create(int capacity)
{
    if (capacity <= 0)
    {
        return NULL;
    }

    PoseRing *r = calloc(1, sizeof(*r));
    if (r == NULL)
    {
        return NULL;
    }

    r->entries = calloc((size_t)capacity, sizeof(*r->entries));
    if (r->entries == NULL)
    {
        free(r);
        return NULL;
    }

    r->capacity  = capacity;
    r->write_pos = 0;
    r->count     = 0;

    if (pthread_mutex_init(&r->mutex, NULL) != 0)
    {
        free(r->entries);
        free(r);
        return NULL;
    }

    return r;
}

void pose_ring_destroy(PoseRing *r)
{
    if (r == NULL)
    {
        return;
    }
    pthread_mutex_destroy(&r->mutex);
    free(r->entries);
    free(r);
}

void pose_ring_record(PoseRing *r, uint32_t frame_id,
                      float hor_angle, float ver_angle,
                      uint64_t capture_ts_ms)
{
    if (r == NULL)
    {
        return;
    }

    pthread_mutex_lock(&r->mutex);

    PoseEntry *slot = &r->entries[r->write_pos];
    slot->frame_id      = frame_id;
    slot->hor_angle     = hor_angle;
    slot->ver_angle     = ver_angle;
    slot->capture_ts_ms = capture_ts_ms;

    r->write_pos = (r->write_pos + 1) % r->capacity;
    r->count++;

    pthread_mutex_unlock(&r->mutex);
}

bool pose_ring_lookup(PoseRing *r, uint32_t frame_id, PoseEntry *out)
{
    if (r == NULL || out == NULL)
    {
        return false;
    }

    bool found = false;

    pthread_mutex_lock(&r->mutex);

    /* Scan newest-first: the matching frame_id is almost always among the
     * most recently recorded entries, so we walk backwards from write_pos.
     * Capacity is tiny (~16) so the full scan is cheap regardless. */
    for (int i = 0; i < r->capacity; i++)
    {
        int idx = (r->write_pos - 1 - i + r->capacity) % r->capacity;
        const PoseEntry *e = &r->entries[idx];

        /* Empty-slot sentinel: calloc leaves capture_ts_ms == 0, so skip those
         * rather than let an unwritten slot match frame_id 0. Assumes no real
         * capture ever timestamps at exactly 0 - true for CLOCK_MONOTONIC ms,
         * which is uptime-based and non-zero by the time capture starts. */
        if (e->capture_ts_ms == 0)
        {
            continue;
        }

        if (e->frame_id == frame_id)
        {
            *out  = *e;
            found = true;
            break;
        }
    }

    pthread_mutex_unlock(&r->mutex);
    return found;
}

uint64_t pose_ring_count(PoseRing *r)
{
    if (r == NULL)
    {
        return 0;
    }

    pthread_mutex_lock(&r->mutex);
    uint64_t c = r->count;
    pthread_mutex_unlock(&r->mutex);
    return c;
}
