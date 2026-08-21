/**
 * test_orin_receiver.c
 *
 * Integration test for the ZeroMQ detection receiver. Spins up a real PULL
 * receiver, pushes a detection over a real PUSH socket, and asserts the
 * handler fires with the detection joined to the right capture-time pose.
 *
 * Needs libzmq.  Build/run:  make test_orin_receiver && ./test_orin_receiver
 */

#include "orin_receiver.h"
#include "pose_ring.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zmq.h>

#define ENDPOINT "tcp://127.0.0.1:5599"

static int g_failures = 0;
#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg,             \
                    __FILE__, __LINE__);                           \
            g_failures++;                                           \
        }                                                          \
    } while (0)

/* Handler captures what it received under a mutex + condvar so the test thread
 * can wait for delivery deterministically. */
typedef struct
{
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    int             calls;
    bool            had_pose;
    PoseEntry       pose;
    OrinDetectionMsg msg;
} Captured;

static void handler(const OrinDetectionMsg *msg, const PoseEntry *pose, void *user)
{
    Captured *c = (Captured *)user;
    pthread_mutex_lock(&c->mtx);
    c->calls++;
    c->msg = *msg;
    c->had_pose = (pose != NULL);
    if (pose) c->pose = *pose;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mtx);
}

static bool wait_for_call(Captured *c, int target_calls, int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&c->mtx);
    while (c->calls < target_calls)
    {
        if (pthread_cond_timedwait(&c->cv, &c->mtx, &deadline) != 0)
        {
            break;  /* timed out */
        }
    }
    bool ok = (c->calls >= target_calls);
    pthread_mutex_unlock(&c->mtx);
    return ok;
}

static uint64_t now_ms_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

int main(void)
{
    Captured cap;
    memset(&cap, 0, sizeof(cap));
    pthread_mutex_init(&cap.mtx, NULL);
    pthread_cond_init(&cap.cv, NULL);

    PoseRing *ring = pose_ring_create(POSE_RING_DEFAULT_CAPACITY);
    CHECK(ring != NULL, "ring create");

    /* Frame 12345 was captured at pan=91.5, tilt=45.0. */
    pose_ring_record(ring, 12345u, 91.5f, 45.0f, now_ms_mono());

    OrinReceiver *rx = orin_receiver_start(ENDPOINT, ring, handler, &cap);
    CHECK(rx != NULL, "receiver start");
    if (rx == NULL) { return 1; }

    /* Orin side: a PUSH socket that connects to the Pi's PULL bind. */
    void *ctx  = zmq_ctx_new();
    void *push = zmq_socket(ctx, ZMQ_PUSH);
    CHECK(zmq_connect(push, ENDPOINT) == 0, "push connect");
    /* PUSH blocks if it connects before the PULL bind is ready; give the
     * receiver a moment and let zmq's connection handshake settle. */
    struct timespec settle = { 0, 200 * 1000000L };
    nanosleep(&settle, NULL);

    /* Message 1: known frame_id -> should join to the recorded pose. */
    const char *m1 =
        "{\"type\":\"target_detection\",\"frame_id\":12345,\"timestamp_ms\":678,"
        "\"detections\":[{\"id\":\"1\",\"class\":\"HUMAN\",\"confidence\":0.85,"
        "\"bbox\":{\"x\":100,\"y\":50,\"width\":200,\"height\":400}}]}";
    CHECK(zmq_send(push, m1, strlen(m1), 0) == (int)strlen(m1), "send m1");

    CHECK(wait_for_call(&cap, 1, 2000), "handler called for m1");
    pthread_mutex_lock(&cap.mtx);
    CHECK(cap.msg.frame_id == 12345u, "m1 frame_id");
    CHECK(cap.msg.num_detections == 1, "m1 detection count");
    CHECK(cap.had_pose, "m1 joined a pose");
    CHECK(cap.pose.hor_angle == 91.5f, "m1 pose pan");
    CHECK(cap.pose.ver_angle == 45.0f, "m1 pose tilt");
    pthread_mutex_unlock(&cap.mtx);

    /* Message 2: unknown frame_id -> handler fires but with NULL pose. */
    const char *m2 =
        "{\"frame_id\":99999,\"detections\":[{\"id\":\"2\",\"confidence\":0.4,"
        "\"bbox\":{\"x\":1,\"y\":1,\"width\":2,\"height\":2}}]}";
    CHECK(zmq_send(push, m2, strlen(m2), 0) == (int)strlen(m2), "send m2");

    CHECK(wait_for_call(&cap, 2, 2000), "handler called for m2");
    pthread_mutex_lock(&cap.mtx);
    CHECK(cap.msg.frame_id == 99999u, "m2 frame_id");
    CHECK(!cap.had_pose, "m2 had no pose (aged out / never recorded)");
    pthread_mutex_unlock(&cap.mtx);

    unsigned long received = 0, parse_fail = 0, pose_miss = 0;
    orin_receiver_stats(rx, &received, &parse_fail, &pose_miss);
    CHECK(received == 2, "stats: 2 received");
    CHECK(parse_fail == 0, "stats: 0 parse failures");
    CHECK(pose_miss == 1, "stats: 1 pose miss");

    zmq_close(push);
    zmq_ctx_term(ctx);
    orin_receiver_stop(rx);
    pose_ring_destroy(ring);

    if (g_failures == 0)
    {
        printf("PASS: all orin_receiver integration tests\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
}
