/*
    TEST 2 - GPS static position scatter.

    The receiver is powered from the Pi and starts acquiring on its own as soon
    as the board comes up, so a genuine cold start cannot be staged on this rig
    and time to first fix is not measured. What is measured is how much the
    reported position wanders while the unit stands still, which is the number
    that matters for the book.

    Place the unit outdoors with a clear view of the sky, wait for a fix, and do
    not move it for the duration of the run.

    Output: test2_gps_scatter.csv
    Usage:  sudo ./test2_gps_scatter [seconds]
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

#define TEST2_OUTPUT_PATH       "test2_gps_scatter.csv"
#define TEST2_DEFAULT_SECONDS   300U
#define TEST2_FIX_TIMEOUT_S     600U
#define TEST2_SAMPLE_PERIOD_MS  1000U

int main(int argc, char** argv)
{
    eStatus         status;
    FILE*           output;
    const DDLFrame* snapshot;
    uint64_t        run_seconds = TEST2_DEFAULT_SECONDS;
    uint64_t        start_us;
    uint64_t        hold_start_us;
    double          elapsed_s;
    uint32_t        samples  = 0U;
    bool            fix_seen = false;

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

    output = fopen(TEST2_OUTPUT_PATH, "w");
    if(output == NULL)
    {
        (void)fprintf(stderr, "Failed to open %s\n", TEST2_OUTPUT_PATH);
        return 1;
    }

    (void)fprintf(output, "sample_index,elapsed_s,fix_type,num_satellites,"
                          "latitude,longitude,h_acc_m\n");

    snapshot = app_get_ddl_snapshot();
    start_us = util_trace_now_us();

    (void)printf("Waiting for a valid fix\n");

    while(!fix_seen)
    {
        osal_delay_ms(TEST2_SAMPLE_PERIOD_MS);

        elapsed_s = (double)(util_trace_now_us() - start_us) / 1000000.0;

        if(snapshot->gps_frame.valid && snapshot->gps_frame.fix_type >= 2U)
        {
            fix_seen = true;
            (void)printf("\nFix acquired, %u satellites. Hold still for %llu s\n",
                         (unsigned int)snapshot->gps_frame.num_satellites,
                         (unsigned long long)run_seconds);
        }
        else if(elapsed_s > (double)TEST2_FIX_TIMEOUT_S)
        {
            (void)fprintf(stderr, "\nNo fix within %u s, aborting\n",
                          TEST2_FIX_TIMEOUT_S);
            break;
        }
        else
        {
            (void)printf("\rwaiting: %.0f s", elapsed_s);
        }
    }

    if(fix_seen)
    {
        hold_start_us = util_trace_now_us();

        while(true)
        {
            osal_delay_ms(TEST2_SAMPLE_PERIOD_MS);

            elapsed_s = (double)(util_trace_now_us() - hold_start_us) / 1000000.0;

            if(elapsed_s > (double)run_seconds)
            {
                break;
            }

            if(snapshot->gps_frame.valid)
            {
                (void)fprintf(output, "%u,%.3f,%u,%u,%.7f,%.7f,%.2f\n",
                              samples,
                              elapsed_s,
                              (unsigned int)snapshot->gps_frame.fix_type,
                              (unsigned int)snapshot->gps_frame.num_satellites,
                              snapshot->gps_frame.latitude,
                              snapshot->gps_frame.longitude,
                              (double)snapshot->gps_frame.h_acc);
                samples++;
            }

            (void)printf("\rhold: %.0f s, samples: %u", elapsed_s, samples);
        }

        (void)printf("\nWrote %u samples to %s\n", samples, TEST2_OUTPUT_PATH);
    }

    (void)fclose(output);

    (void)app_end();
    app_join();
    app_delete();
    util_event_bus_delete();
    hal_cleanup();
    log_exit();

    return 0;
}
