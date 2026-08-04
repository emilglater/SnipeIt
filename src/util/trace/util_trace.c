#include "util_trace.h"

/* Standard library includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

uint64_t util_trace_now_us(void)
{
    struct timespec now;

    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }

    return ((uint64_t)now.tv_sec * 1000000U) + ((uint64_t)now.tv_nsec / 1000U);
}

#if UTIL_TRACE_ENABLED

/* User library includes */
#include "osal/osal.h"

/* One completed post-to-dispatch measurement. Member order keeps the struct
 * free of padding, which the build enforces with -Wpadded. */
typedef struct
{
    uint64_t post_ts_us;
    uint64_t dispatch_ts_us;
    uint64_t latency_us;
    uint32_t event_type;
    uint32_t ao_index;
} UtilTraceSample;

/* Timestamps of posts to one Active Object that have not been dispatched yet. */
typedef struct
{
    const void* active_object;
    uint64_t    pending[eUTIL_TRACE_PENDING_SLOTS];
    uint32_t    head;
    uint32_t    tail;
} UtilTraceAO;

static UtilTraceSample samples[eUTIL_TRACE_MAX_SAMPLES];
static UtilTraceAO     tracked[eUTIL_TRACE_MAX_AO];

static uint32_t sample_count;
static uint32_t tracked_count;
static void*    trace_mutex;

/* Caller must hold trace_mutex. Returns NULL when the table is full. */
static UtilTraceAO* trace_find_ao(const void* active_object, bool create)
{
    uint32_t i;

    for(i = 0U; i < tracked_count; i++)
    {
        if(tracked[i].active_object == active_object)
        {
            return &tracked[i];
        }
    }

    if(!create || tracked_count >= (uint32_t)eUTIL_TRACE_MAX_AO)
    {
        return NULL;
    }

    tracked[tracked_count].active_object = active_object;
    tracked[tracked_count].head          = 0U;
    tracked[tracked_count].tail          = 0U;
    tracked_count++;

    return &tracked[tracked_count - 1U];
}

eStatus util_trace_init(void)
{
    sample_count  = 0U;
    tracked_count = 0U;

    if(osal_mutex_init(&trace_mutex))
    {
        return eSTATUS_SYSTEM_ERROR;
    }

    return eSTATUS_SUCCESSFUL;
}

void util_trace_ao_post(const void* active_object, uint32_t event_type)
{
    UtilTraceAO* entry;
    uint32_t   next;

    (void)event_type;

    if(trace_mutex == NULL || active_object == NULL)
    {
        return;
    }

    osal_mutex_lock(trace_mutex);

    entry = trace_find_ao(active_object, true);
    if(entry != NULL)
    {
        next = (entry->head + 1U) % (uint32_t)eUTIL_TRACE_PENDING_SLOTS;
        if(next != entry->tail)
        {
            entry->pending[entry->head] = util_trace_now_us();
            entry->head                 = next;
        }
    }

    osal_mutex_unlock(trace_mutex);
}

void util_trace_ao_post_cancel(const void* active_object)
{
    UtilTraceAO* entry;

    if(trace_mutex == NULL || active_object == NULL)
    {
        return;
    }

    osal_mutex_lock(trace_mutex);

    entry = trace_find_ao(active_object, false);
    if(entry != NULL && entry->head != entry->tail)
    {
        entry->head = (entry->head + (uint32_t)eUTIL_TRACE_PENDING_SLOTS - 1U) %
                      (uint32_t)eUTIL_TRACE_PENDING_SLOTS;
    }

    osal_mutex_unlock(trace_mutex);
}

void util_trace_ao_dispatch(const void* active_object, uint32_t event_type)
{
    UtilTraceAO* entry;
    uint64_t   dispatch_ts;
    uint64_t   post_ts;

    if(trace_mutex == NULL || active_object == NULL)
    {
        return;
    }

    dispatch_ts = util_trace_now_us();

    osal_mutex_lock(trace_mutex);

    entry = trace_find_ao(active_object, false);
    if(entry != NULL && entry->tail != entry->head &&
       sample_count < (uint32_t)eUTIL_TRACE_MAX_SAMPLES)
    {
        post_ts     = entry->pending[entry->tail];
        entry->tail = (entry->tail + 1U) % (uint32_t)eUTIL_TRACE_PENDING_SLOTS;

        samples[sample_count].post_ts_us     = post_ts;
        samples[sample_count].dispatch_ts_us = dispatch_ts;
        samples[sample_count].latency_us     = dispatch_ts - post_ts;
        samples[sample_count].event_type     = event_type;
        samples[sample_count].ao_index       = (uint32_t)(entry - tracked);
        sample_count++;
    }

    osal_mutex_unlock(trace_mutex);
}

uint32_t util_trace_sample_count(void)
{
    uint32_t count;

    if(trace_mutex == NULL)
    {
        return 0U;
    }

    osal_mutex_lock(trace_mutex);
    count = sample_count;
    osal_mutex_unlock(trace_mutex);

    return count;
}

eStatus util_trace_dump_samples(const char* path)
{
    FILE*    file;
    uint32_t i;

    if(path == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    file = fopen(path, "w");
    if(file == NULL)
    {
        return eSTATUS_ACTION_FAILED;
    }

    (void)fprintf(file, "sample_index,ao_index,event_type,post_ts_us,dispatch_ts_us,latency_us\n");

    osal_mutex_lock(trace_mutex);
    for(i = 0U; i < sample_count; i++)
    {
        (void)fprintf(file, "%u,%u,%u,%llu,%llu,%llu\n",
                      i,
                      samples[i].ao_index,
                      samples[i].event_type,
                      (unsigned long long)samples[i].post_ts_us,
                      (unsigned long long)samples[i].dispatch_ts_us,
                      (unsigned long long)samples[i].latency_us);
    }
    osal_mutex_unlock(trace_mutex);

    (void)fclose(file);

    return eSTATUS_SUCCESSFUL;
}

void util_trace_delete(void)
{
    if(trace_mutex != NULL)
    {
        osal_mutex_destroy(trace_mutex);
        trace_mutex = NULL;
    }
}

#endif
