#ifndef UTIL_TRACE_H
#define UTIL_TRACE_H

/* Standard library includes */
#include <stdint.h>

/* User library includes */
#include "util/trace/util_trace_config.h"
#include "status.h"

/**
 * @brief   Read the monotonic clock in microseconds.
 * @details Always available, also when instrumentation is disabled, because
 *          the test runners use it for their own pacing.
 * @returns Microseconds since an unspecified fixed point in the past.
 */
uint64_t util_trace_now_us(void);

#if UTIL_TRACE_ENABLED

/**
 * @brief   Initialize the trace buffers.
 * @details Must be called once, before any Active Object is created.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_SYSTEM_ERROR    mutex initialization failed
 */
eStatus util_trace_init(void);

/**
 * @brief   Record the moment an event is submitted to an AO queue.
 * @details MUST be called BEFORE the queue push. If it is called after, the AO
 *          thread can pop and dispatch the event before the post is recorded,
 *          the dispatch then finds nothing pending and is dropped, and every
 *          later pair for that Active Object is shifted by one event.
 * @param   active_object The Active Object the event is being posted to.
 * @param   event_type The event type being posted.
 */
void util_trace_ao_post(const void* active_object, uint32_t event_type);

/**
 * @brief   Undo the most recent post record for an Active Object.
 * @details Called when the queue push that follows the post record fails, so
 *          that no unmatched entry is left behind.
 * @param   active_object The Active Object whose last post record to drop.
 */
void util_trace_ao_post_cancel(const void* active_object);

/**
 * @brief   Record the moment an AO thread starts handling an event.
 * @details Pairs with the oldest unmatched post for the same Active Object,
 *          which is correct because the AO queue is FIFO.
 * @param   active_object The Active Object that is dispatching.
 * @param   event_type The event type being dispatched.
 */
void util_trace_ao_dispatch(const void* active_object, uint32_t event_type);

/**
 * @brief   Number of complete latency samples collected so far.
 */
uint32_t util_trace_sample_count(void);

/**
 * @brief   Write the collected latency samples to a CSV file.
 * @param   path Destination file path.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      path is NULL
 * @retval  eSTATUS_ACTION_FAILED   the file could not be opened
 */
eStatus util_trace_dump_samples(const char* path);

/**
 * @brief   Release the trace resources.
 */
void util_trace_delete(void);

#else

#define util_trace_init()                       (eSTATUS_SUCCESSFUL)
#define util_trace_ao_post(ao, type)            ((void)(ao), (void)(type))
#define util_trace_ao_post_cancel(ao)           ((void)(ao))
#define util_trace_ao_dispatch(ao, type)        ((void)(ao), (void)(type))
#define util_trace_sample_count()               (0U)
#define util_trace_dump_samples(path)           ((void)(path), eSTATUS_SUCCESSFUL)
#define util_trace_delete()                     ((void)0)

#endif

#endif
