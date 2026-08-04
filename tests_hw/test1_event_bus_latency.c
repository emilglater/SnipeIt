/*
    TEST 1 - Event dispatch latency of the Active Object framework.

    Measures the time between the moment an event is accepted into an Active
    Object queue and the moment the AO thread begins handling it. The whole
    system runs normally while measuring, so the numbers reflect real
    contention between all DDL modules, the scheduler and the broadcaster.

    Output: test1_event_bus_latency.csv
    Usage:  sudo ./test1_event_bus_latency [seconds]
*/

/* Standard library includes */
#include <stdio.h>
#include <stdlib.h>

/* User library includes */
#include "util/event_bus/event_list.h"
#include "util/event_bus/event_bus.h"
#include "util/trace/util_trace.h"
#include "util/log/log.h"
#include "osal/osal.h"
#include "hal/hal.h"
#include "app/app.h"

#define TEST1_OUTPUT_PATH       "test1_event_bus_latency.csv"
#define TEST1_DEFAULT_SECONDS   60U
#define TEST1_TARGET_SAMPLES    100U
#define TEST1_POLL_MS           250U

int main(int argc, char** argv)
{
    eStatus  status;
    uint64_t run_seconds     = TEST1_DEFAULT_SECONDS;
    uint64_t deadline_us;
    uint32_t collected       = 0U;

    /* libc block-buffers stdout when it is redirected to a file, which makes
     * progress output appear in bursts and hides where the run actually is. */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);

    if(argc > 1)
    {
        run_seconds = (uint64_t)strtoul(argv[1], NULL, 10);
    }

    status = log_init();
    if(status)
    {
        (void)fprintf(stderr, "Failed to initialize the log\n");
        return 1;
    }

    status = hal_init();
    if(status)
    {
        (void)fprintf(stderr, "Failed to initialize the HAL layer\n");
        return 1;
    }

    /* Must run before app_init, because app_init creates the Active Objects. */
    status = util_trace_init();
    if(status)
    {
        (void)fprintf(stderr, "Failed to initialize the trace buffers\n");
        return 1;
    }

    status = util_event_bus_init();
    if(status)
    {
        (void)fprintf(stderr, "Failed to initialize the event bus\n");
        return 1;
    }

    status = app_init();
    if(status)
    {
        (void)fprintf(stderr, "Failed to initialize the APP layer\n");
        return 1;
    }

    status = util_event_bus_publish(eAO_SCHEDULER,
                                    (uint32_t)eSCHEDULER_EVENT_START);
    if(status)
    {
        (void)fprintf(stderr, "Failed to start the scheduler\n");
        return 1;
    }

    (void)printf("Collecting event latency samples for %llu seconds\n",
                 (unsigned long long)run_seconds);

    deadline_us = util_trace_now_us() + (run_seconds * 1000000U);

    while(util_trace_now_us() < deadline_us)
    {
        osal_delay_ms(TEST1_POLL_MS);
        collected = util_trace_sample_count();
        (void)printf("\rsamples: %u", collected);
    }

    (void)printf("\n");

    if(collected < TEST1_TARGET_SAMPLES)
    {
        (void)fprintf(stderr,
                      "WARNING: only %u samples collected, %u were requested. "
                      "Run for longer.\n",
                      collected, TEST1_TARGET_SAMPLES);
    }

    status = util_trace_dump_samples(TEST1_OUTPUT_PATH);
    if(status)
    {
        (void)fprintf(stderr, "Failed to write %s\n", TEST1_OUTPUT_PATH);
    }
    else
    {
        (void)printf("Wrote %u samples to %s\n", collected, TEST1_OUTPUT_PATH);
    }

    (void)app_end();
    app_join();
    app_delete();
    util_event_bus_delete();
    util_trace_delete();
    hal_cleanup();
    log_exit();

    return 0;
}
