/**
 * orin_receiver.c
 *
 * ZeroMQ PULL receiver thread. See orin_receiver.h for the contract.
 *
 * Build dependency: libzmq (link with -lzmq; headers from libzmq3-dev).
 */

#include "orin_receiver.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <zmq.h>

/* How long zmq_poll() blocks before the loop re-checks the stop flag. Keeps
 * shutdown latency bounded without busy-spinning. */
#define ORIN_POLL_TIMEOUT_MS 100

struct OrinReceiver
{
    void                *zmq_ctx;
    void                *sock;
    PoseRing            *ring;        /* borrowed */
    OrinDetectionHandler handler;
    void                *user;

    pthread_t            tid;
    bool                 thread_started;
    volatile bool        running;

    /* Diagnostics — written only by the receive thread, read racily by
     * orin_receiver_stats(); plain unsigned long is fine for counters. */
    unsigned long        n_received;
    unsigned long        n_parse_failures;
    unsigned long        n_pose_misses;
};

static void handle_one(OrinReceiver *r, const char *data, size_t size)
{
    r->n_received++;

    OrinDetectionMsg msg;
    if (!orin_detection_msg_parse(data, size, &msg))
    {
        r->n_parse_failures++;
        fprintf(stderr, "[ORIN-RX] dropped unparseable message (%zu bytes)\n", size);
        return;
    }

    PoseEntry pose;
    bool have_pose = msg.has_frame_id &&
                     pose_ring_lookup(r->ring, msg.frame_id, &pose);
    if (!have_pose)
    {
        r->n_pose_misses++;
    }

    if (r->handler != NULL)
    {
        r->handler(&msg, have_pose ? &pose : NULL, r->user);
    }
}

static void *receive_loop(void *arg)
{
    OrinReceiver *r = (OrinReceiver *)arg;

    zmq_pollitem_t items[1];
    items[0].socket = r->sock;
    items[0].fd     = 0;
    items[0].events = ZMQ_POLLIN;

    while (r->running)
    {
        int rc = zmq_poll(items, 1, ORIN_POLL_TIMEOUT_MS);
        if (rc < 0)
        {
            if (zmq_errno() == EINTR)
            {
                continue;
            }
            fprintf(stderr, "[ORIN-RX] zmq_poll failed: %s\n", zmq_strerror(zmq_errno()));
            break;
        }
        if (rc == 0 || !(items[0].revents & ZMQ_POLLIN))
        {
            continue;  /* timeout — loop back and re-check r->running */
        }

        /* Drain everything currently queued before polling again, so a burst
         * does not back up. ZMQ_DONTWAIT returns EAGAIN once drained. */
        for (;;)
        {
            zmq_msg_t zm;
            zmq_msg_init(&zm);

            int n = zmq_msg_recv(&zm, r->sock, ZMQ_DONTWAIT);
            if (n < 0)
            {
                zmq_msg_close(&zm);
                if (zmq_errno() == EAGAIN)
                {
                    break;  /* nothing left right now */
                }
                if (zmq_errno() == EINTR)
                {
                    continue;
                }
                fprintf(stderr, "[ORIN-RX] zmq_msg_recv failed: %s\n",
                        zmq_strerror(zmq_errno()));
                break;
            }

            handle_one(r, (const char *)zmq_msg_data(&zm), zmq_msg_size(&zm));
            zmq_msg_close(&zm);
        }
    }

    return NULL;
}

OrinReceiver *orin_receiver_start(const char *endpoint, PoseRing *ring,
                                  OrinDetectionHandler handler, void *user)
{
    if (endpoint == NULL || ring == NULL || handler == NULL)
    {
        return NULL;
    }

    OrinReceiver *r = calloc(1, sizeof(*r));
    if (r == NULL)
    {
        return NULL;
    }
    r->ring    = ring;
    r->handler = handler;
    r->user    = user;

    r->zmq_ctx = zmq_ctx_new();
    if (r->zmq_ctx == NULL)
    {
        fprintf(stderr, "[ORIN-RX] zmq_ctx_new failed\n");
        free(r);
        return NULL;
    }

    r->sock = zmq_socket(r->zmq_ctx, ZMQ_PULL);
    if (r->sock == NULL)
    {
        fprintf(stderr, "[ORIN-RX] zmq_socket failed: %s\n", zmq_strerror(zmq_errno()));
        zmq_ctx_term(r->zmq_ctx);
        free(r);
        return NULL;
    }

    /* Bound the inbound queue at 100 messages. NOTE: this does NOT drop.
     * ZeroMQ only discards at the high-water mark on PUB/SUB; on PUSH/PULL it
     * applies back-pressure, so at the mark the Orin's send blocks rather than
     * the Pi dropping anything. Detections are only useful fresh, so if we
     * ever fall behind the backlog surfaces downstream as pose-ring misses,
     * not as drops here. */
    int hwm = 100;
    zmq_setsockopt(r->sock, ZMQ_RCVHWM, &hwm, sizeof(hwm));

    if (zmq_bind(r->sock, endpoint) != 0)
    {
        fprintf(stderr, "[ORIN-RX] zmq_bind(%s) failed: %s\n",
                endpoint, zmq_strerror(zmq_errno()));
        zmq_close(r->sock);
        zmq_ctx_term(r->zmq_ctx);
        free(r);
        return NULL;
    }

    r->running = true;
    if (pthread_create(&r->tid, NULL, receive_loop, r) != 0)
    {
        fprintf(stderr, "[ORIN-RX] pthread_create failed\n");
        r->running = false;
        zmq_close(r->sock);
        zmq_ctx_term(r->zmq_ctx);
        free(r);
        return NULL;
    }
    r->thread_started = true;

    printf("[ORIN-RX] Listening for detections on %s\n", endpoint);
    return r;
}

void orin_receiver_stop(OrinReceiver *r)
{
    if (r == NULL)
    {
        return;
    }

    r->running = false;
    if (r->thread_started)
    {
        pthread_join(r->tid, NULL);
    }

    /* Close the socket before terminating the context, otherwise zmq_ctx_term
     * blocks waiting for the socket to close. */
    if (r->sock != NULL)
    {
        zmq_close(r->sock);
    }
    if (r->zmq_ctx != NULL)
    {
        zmq_ctx_term(r->zmq_ctx);
    }

    printf("[ORIN-RX] Stopped (received=%lu parse_fail=%lu pose_miss=%lu)\n",
           r->n_received, r->n_parse_failures, r->n_pose_misses);
    free(r);
}

void orin_receiver_stats(const OrinReceiver *r,
                         unsigned long *received,
                         unsigned long *parse_failures,
                         unsigned long *pose_misses)
{
    if (r == NULL)
    {
        return;
    }
    if (received)       *received       = r->n_received;
    if (parse_failures) *parse_failures = r->n_parse_failures;
    if (pose_misses)    *pose_misses    = r->n_pose_misses;
}
