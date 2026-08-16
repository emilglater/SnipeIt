/**
 * test_pose_ring.c
 *
 * Standalone unit test for the frame_id -> pose ring buffer.
 * No test framework: asserts + a final summary line. Exit code is non-zero
 * on any failure so it can gate a build.
 *
 * Build/run:
 *   make test_pose_ring && ./test_pose_ring
 */

#include "pose_ring.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg,             \
                    __FILE__, __LINE__);                           \
            g_failures++;                                           \
        }                                                          \
    } while (0)

static uint64_t now_ms_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* Basic record then look the same frame_id back up. */
static void test_basic_record_lookup(void)
{
    PoseRing *r = pose_ring_create(POSE_RING_DEFAULT_CAPACITY);
    CHECK(r != NULL, "create");

    pose_ring_record(r, 42u, 91.5f, 45.0f, now_ms_mono());

    PoseEntry e;
    CHECK(pose_ring_lookup(r, 42u, &e), "lookup hit for recorded id");
    CHECK(e.frame_id == 42u, "frame_id matches");
    CHECK(e.hor_angle == 91.5f, "hor_angle matches");
    CHECK(e.ver_angle == 45.0f, "ver_angle matches");

    CHECK(!pose_ring_lookup(r, 43u, &e), "miss for never-recorded id");
    CHECK(pose_ring_count(r) == 1, "count == 1");

    pose_ring_destroy(r);
}

/* Once the ring wraps, the oldest frame_id must be evicted (a stale detection
 * referencing it misses), while the newest is still present. */
static void test_eviction(void)
{
    const int cap = 8;
    PoseRing *r = pose_ring_create(cap);

    /* Record cap+3 frames -> ids 0,1,2 are overwritten. */
    for (uint32_t id = 0; id < (uint32_t)cap + 3u; id++)
    {
        pose_ring_record(r, id, (float)id, (float)id, now_ms_mono());
    }

    PoseEntry e;
    CHECK(!pose_ring_lookup(r, 0u, &e), "evicted oldest (0) misses");
    CHECK(!pose_ring_lookup(r, 2u, &e), "evicted (2) misses");
    CHECK(pose_ring_lookup(r, 3u, &e), "still-present (3) hits");
    CHECK(pose_ring_lookup(r, (uint32_t)cap + 2u, &e), "newest hits");
    CHECK(e.hor_angle == (float)(cap + 2), "newest payload intact");
    CHECK(pose_ring_count(r) == (uint64_t)cap + 3u, "monotonic count");

    pose_ring_destroy(r);
}

/* Empty buffer must not spuriously match frame_id 0 (calloc'd slots). */
static void test_empty_no_false_match_on_zero(void)
{
    PoseRing *r = pose_ring_create(4);
    PoseEntry e;
    CHECK(!pose_ring_lookup(r, 0u, &e), "empty buffer misses id 0");
    /* ...but once 0 is genuinely recorded it must hit. */
    pose_ring_record(r, 0u, 10.0f, 20.0f, now_ms_mono());
    CHECK(pose_ring_lookup(r, 0u, &e), "recorded id 0 hits");
    CHECK(e.hor_angle == 10.0f, "id 0 payload intact");
    pose_ring_destroy(r);
}

/* Concurrent writer + reader: the writer races a real round-trip lag by
 * looking up a frame_id a few behind the most recent one. No assertion on
 * hit-rate (timing dependent); this is a TSan/own-eyes data-race smoke test
 * that must not crash or trip the mutex. */
typedef struct { PoseRing *r; volatile int stop; volatile uint32_t latest; } SharedT;

static void *writer_thread(void *arg)
{
    SharedT *s = arg;
    for (uint32_t id = 1; !s->stop; id++)
    {
        pose_ring_record(s->r, id, (float)(id % 180), (float)(id % 90), now_ms_mono());
        s->latest = id;
    }
    return NULL;
}

static void test_concurrent(void)
{
    PoseRing *r = pose_ring_create(POSE_RING_DEFAULT_CAPACITY);
    SharedT s = { .r = r, .stop = 0, .latest = 0 };

    pthread_t wt;
    pthread_create(&wt, NULL, writer_thread, &s);

    int hits = 0, attempts = 0;
    for (int i = 0; i < 200000; i++)
    {
        uint32_t latest = s.latest;
        if (latest < 4) continue;
        PoseEntry e;
        attempts++;
        if (pose_ring_lookup(r, latest - 3u, &e))  /* mimic ~3-frame lag */
        {
            hits++;
            CHECK(e.frame_id == latest - 3u, "concurrent hit returns right id");
        }
    }

    s.stop = 1;
    pthread_join(wt, NULL);
    printf("  concurrent: %d/%d lookups hit (timing-dependent, informational)\n",
           hits, attempts);
    CHECK(pose_ring_count(r) > 0, "writer made progress");

    pose_ring_destroy(r);
}

int main(void)
{
    test_basic_record_lookup();
    test_eviction();
    test_empty_no_false_match_on_zero();
    test_concurrent();

    if (g_failures == 0)
    {
        printf("PASS: all pose_ring tests\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
}
