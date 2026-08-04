/*
    TEST 3 - Horizontal axis accuracy, repeatability and hysteresis.

    Only the horizontal axis is measured. It is the axis that carries the whole
    tilt assembly, so it is where the friction of the printed arm shows up, and
    it is the axis whose servo was replaced with a higher torque part.

    The angle reference is the compass, which rides on the rotating assembly, so
    its heading changes one for one with the pan angle. That removes the need to
    read a protractor by hand. AUTO mode checks that this coupling really exists
    before it collects anything, and stops with an explanation if it does not.

    Hysteresis and repeatability come out of the raw headings with no
    calibration at all, because both are differences between headings measured
    at the same commanded angle, so any fixed offset cancels.

    Modes:
        auto    compass referenced sweeps, unattended, this is the one to use
        manual  writes a CSV template to fill in by hand, for a protractor
        scan    times one full sweep of the scan pattern, unattended

    Output: test3_turret_hor_auto.csv, test3_turret_hor_manual.csv,
            test3_scan_duration.txt
    Usage:  sudo ./test3_turret_accuracy <auto|manual|scan> [passes]
*/

/* Standard library includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* User library includes */
#include "util/event_bus/event_list.h"
#include "util/event_bus/event_bus.h"
#include "ddl/compass/compass_config.h"
#include "ddl/servo/servo_config.h"
#include "ddl/servo/servo_events.h"
#include "util/trace/util_trace.h"
#include "ddl/compass/compass.h"
#include "ddl/servo/servo.h"
#include "util/log/log.h"
#include "osal/osal.h"
#include "hal/hal.h"
#include "ddl/ddl.h"

#define TEST3_SETTLE_MS         2000U
#define TEST3_READ_PERIOD_MS    200U
#define TEST3_READS_PER_POINT   5U
#define TEST3_SCAN_TICK_MS      200U
#define TEST3_SCAN_TIMEOUT_S    600U
#define TEST3_PARK_ANGLE        90.0f
#define TEST3_ANGLE_EPSILON     0.5f
#define TEST3_DEFAULT_PASSES    5U
#define TEST3_COUPLING_MIN_DEG  20.0f

static const float test3_angles[] = { 20.0f, 40.0f, 70.0f, 90.0f,
                                      110.0f, 140.0f, 160.0f };

#define TEST3_ANGLE_COUNT (sizeof(test3_angles) / sizeof(test3_angles[0]))

static DDLFrame test3_frame;

/* Events are posted through the event bus rather than through the module post
 * functions directly. The queue stores the POINTER to the event, not a copy, so
 * an Event declared on the stack of a helper like this one would dangle the
 * moment the helper returns. The bus posts each module's own registered event,
 * which has static lifetime. */
static eStatus test3_post_servo(uint32_t event_type)
{
    return util_event_bus_publish(eAO_SERVO, event_type);
}

static eStatus test3_post_compass(void)
{
    return util_event_bus_publish(eAO_COMPASS, (uint32_t)eCOMPASS_EVENT_READ);
}

/* Shortest signed distance from b to a, in the range -180 to 180. */
static float test3_heading_delta(float a, float b)
{
    float delta = a - b;

    while(delta > 180.0f)
    {
        delta -= 360.0f;
    }
    while(delta < -180.0f)
    {
        delta += 360.0f;
    }

    return delta;
}

/* Drives the horizontal axis and waits for the movement to finish. */
static void test3_goto(float hor_angle)
{
    if(ddl_servo_set_target(hor_angle, TEST3_PARK_ANGLE))
    {
        (void)fprintf(stderr, "Rejected target %.1f, outside the legal range\n",
                      (double)hor_angle);
        return;
    }

    (void)test3_post_servo((uint32_t)eSERVO_EVENT_TARGET_UPDATE);
    osal_delay_ms(TEST3_SETTLE_MS);
}

/* Median of several compass reads, which suppresses the odd noisy sample. */
static bool test3_read_heading(float* heading)
{
    float    values[TEST3_READS_PER_POINT];
    float    swap;
    uint32_t count = 0U;
    uint32_t i;
    uint32_t j;

    for(i = 0U; i < (uint32_t)TEST3_READS_PER_POINT; i++)
    {
        (void)test3_post_compass();
        osal_delay_ms(TEST3_READ_PERIOD_MS);

        if(test3_frame.mag_frame.valid)
        {
            values[count] = test3_frame.mag_frame.heading_deg;
            count++;
        }
    }

    if(count == 0U)
    {
        return false;
    }

    for(i = 0U; i < count; i++)
    {
        for(j = i + 1U; j < count; j++)
        {
            if(values[j] < values[i])
            {
                swap      = values[i];
                values[i] = values[j];
                values[j] = swap;
            }
        }
    }

    *heading = values[count / 2U];

    return true;
}

/* Confirms that turning the pan axis actually moves the compass heading.
 * If the compass is bolted to the base rather than the rotating assembly it
 * cannot serve as the angle reference, and there is no point collecting data. */
static bool test3_check_coupling(void)
{
    float low_heading  = 0.0f;
    float high_heading = 0.0f;
    float swing;

    (void)printf("Checking that the compass turns with the pan axis\n");

    test3_goto(SERVO_HORIZONTAL_MIN_ANGLE_DEG);
    if(!test3_read_heading(&low_heading))
    {
        (void)fprintf(stderr, "No valid compass reading, cannot continue\n");
        return false;
    }

    test3_goto(SERVO_HORIZONTAL_MAX_ANGLE_DEG);
    if(!test3_read_heading(&high_heading))
    {
        (void)fprintf(stderr, "No valid compass reading, cannot continue\n");
        return false;
    }

    swing = test3_heading_delta(high_heading, low_heading);
    if(swing < 0.0f)
    {
        swing = -swing;
    }

    (void)printf("Heading at %.0f deg: %.2f, at %.0f deg: %.2f, moved %.1f deg\n",
                 (double)SERVO_HORIZONTAL_MIN_ANGLE_DEG, (double)low_heading,
                 (double)SERVO_HORIZONTAL_MAX_ANGLE_DEG, (double)high_heading,
                 (double)swing);

    if(swing < TEST3_COUPLING_MIN_DEG)
    {
        (void)fprintf(stderr,
                      "The heading barely moved. Either the compass is not turning\n"
                      "with the pan axis, or it is not sampling and the two readings\n"
                      "above are the same stale value. If the two readings are\n"
                      "identical to the decimal, suspect the second case.\n"
                      "Otherwise use manual mode with a protractor.\n");
        return false;
    }

    return true;
}

static void test3_run_auto(uint32_t passes)
{
    FILE*    output;
    float    commanded;
    float    heading  = 0.0f;
    float    hor_pose = 0.0f;
    float    ver_pose = 0.0f;
    uint32_t pass;
    size_t   i;

    (void)printf("Running %u passes, about %u seconds in total\n",
                 passes, 6U + (passes * 46U));

    if(!test3_check_coupling())
    {
        return;
    }

    output = fopen("test3_turret_hor_auto.csv", "w");
    if(output == NULL)
    {
        (void)fprintf(stderr, "Failed to open the output file\n");
        return;
    }

    (void)fprintf(output, "pass_index,commanded_deg,approach_direction,"
                          "pose_deg,heading_deg\n");

    for(pass = 0U; pass < passes; pass++)
    {
        (void)printf("Pass %u of %u, ascending\n", pass + 1U, passes);

        /* Park below the range so every point is reached from a smaller angle. */
        test3_goto(SERVO_HORIZONTAL_MIN_ANGLE_DEG);

        for(i = 0U; i < TEST3_ANGLE_COUNT; i++)
        {
            commanded = test3_angles[i];
            test3_goto(commanded);

            if(!test3_read_heading(&heading))
            {
                continue;
            }

            (void)ddl_servo_get_pose(&hor_pose, &ver_pose);
            (void)fprintf(output, "%u,%.1f,up,%.1f,%.2f\n",
                          pass, (double)commanded,
                          (double)hor_pose, (double)heading);
            (void)printf("  %5.1f deg commanded, heading %.2f\n",
                         (double)commanded, (double)heading);
        }

        (void)printf("Pass %u of %u, descending\n", pass + 1U, passes);

        test3_goto(SERVO_HORIZONTAL_MAX_ANGLE_DEG);

        for(i = TEST3_ANGLE_COUNT; i > 0U; i--)
        {
            commanded = test3_angles[i - 1U];
            test3_goto(commanded);

            if(!test3_read_heading(&heading))
            {
                continue;
            }

            (void)ddl_servo_get_pose(&hor_pose, &ver_pose);
            (void)fprintf(output, "%u,%.1f,down,%.1f,%.2f\n",
                          pass, (double)commanded,
                          (double)hor_pose, (double)heading);
            (void)printf("  %5.1f deg commanded, heading %.2f\n",
                         (double)commanded, (double)heading);
        }
    }

    (void)fclose(output);
    (void)printf("Done. Results in test3_turret_hor_auto.csv\n");
}

static void test3_wait_for_enter(void)
{
    int character;

    do
    {
        character = getchar();
    }
    while(character != '\n' && character != EOF);
}

static void test3_run_manual(void)
{
    FILE*  output;
    float  commanded;
    size_t i;

    output = fopen("test3_turret_hor_manual.csv", "w");
    if(output == NULL)
    {
        (void)fprintf(stderr, "Failed to open the output file\n");
        return;
    }

    (void)fprintf(output, "pass_index,commanded_deg,approach_direction,"
                          "pose_deg,measured_deg\n");

    test3_goto(SERVO_HORIZONTAL_MIN_ANGLE_DEG);

    for(i = 0U; i < TEST3_ANGLE_COUNT; i++)
    {
        commanded = test3_angles[i];
        test3_goto(commanded);
        (void)printf("Ascending, commanded %.1f deg. Read the scale, press ENTER.\n",
                     (double)commanded);
        test3_wait_for_enter();
        (void)fprintf(output, "0,%.1f,up,%.1f,\n",
                      (double)commanded, (double)commanded);
    }

    test3_goto(SERVO_HORIZONTAL_MAX_ANGLE_DEG);

    for(i = TEST3_ANGLE_COUNT; i > 0U; i--)
    {
        commanded = test3_angles[i - 1U];
        test3_goto(commanded);
        (void)printf("Descending, commanded %.1f deg. Read the scale, press ENTER.\n",
                     (double)commanded);
        test3_wait_for_enter();
        (void)fprintf(output, "0,%.1f,down,%.1f,\n",
                      (double)commanded, (double)commanded);
    }

    (void)fclose(output);
    (void)printf("Template written. Fill in the measured_deg column.\n");
}

/* Drives the scan pattern and times one full sweep, from the start corner
 * until the pattern returns to it. */
static void test3_run_scan(void)
{
    FILE*    output;
    float    hor_pose    = 0.0f;
    float    ver_pose    = 0.0f;
    uint64_t start_us;
    uint64_t elapsed_us;
    uint32_t steps       = 0U;
    bool     left_origin = false;
    bool     completed   = false;

    (void)test3_post_servo((uint32_t)eSERVO_EVENT_DIRECTIONS);
    osal_delay_ms(TEST3_SETTLE_MS);

    start_us = util_trace_now_us();

    while(!completed)
    {
        (void)test3_post_servo((uint32_t)eSERVO_EVENT_DIRECTIONS);
        osal_delay_ms(TEST3_SCAN_TICK_MS);
        steps++;

        if(ddl_servo_get_pose(&hor_pose, &ver_pose))
        {
            continue;
        }

        if(!left_origin)
        {
            if(hor_pose > (SERVO_HORIZONTAL_MIN_ANGLE_DEG + TEST3_ANGLE_EPSILON) ||
               ver_pose > (SERVO_VERTICAL_MIN_ANGLE_DEG + TEST3_ANGLE_EPSILON))
            {
                left_origin = true;
            }
        }
        else if(hor_pose <= (SERVO_HORIZONTAL_MIN_ANGLE_DEG + TEST3_ANGLE_EPSILON) &&
                ver_pose <= (SERVO_VERTICAL_MIN_ANGLE_DEG + TEST3_ANGLE_EPSILON))
        {
            completed = true;
        }
        else
        {
            /* still sweeping */
        }

        elapsed_us = util_trace_now_us() - start_us;
        if(elapsed_us > ((uint64_t)TEST3_SCAN_TIMEOUT_S * 1000000U))
        {
            (void)fprintf(stderr, "Scan did not return to the origin within %u s\n",
                          TEST3_SCAN_TIMEOUT_S);
            break;
        }
    }

    elapsed_us = util_trace_now_us() - start_us;

    output = fopen("test3_scan_duration.txt", "w");
    if(output != NULL)
    {
        (void)fprintf(output,
                      "full_scan_seconds=%.2f\nsteps=%u\ntick_ms=%u\ncompleted=%s\n",
                      (double)elapsed_us / 1000000.0,
                      steps,
                      TEST3_SCAN_TICK_MS,
                      completed ? "yes" : "no");
        (void)fclose(output);
    }

    (void)printf("Full scan took %.2f s over %u steps\n",
                 (double)elapsed_us / 1000000.0, steps);
}

int main(int argc, char** argv)
{
    eStatus  status;
    uint32_t passes    = TEST3_DEFAULT_PASSES;
    bool     run_auto   = false;
    bool     run_manual = false;
    bool     run_scan   = false;

    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);

    if(argc < 2)
    {
        (void)fprintf(stderr, "Usage: %s <auto|manual|scan> [passes]\n", argv[0]);
        return 1;
    }

    if(strcmp(argv[1], "auto") == 0)
    {
        run_auto = true;
    }
    else if(strcmp(argv[1], "manual") == 0)
    {
        run_manual = true;
    }
    else if(strcmp(argv[1], "scan") == 0)
    {
        run_scan = true;
    }
    else
    {
        (void)fprintf(stderr, "Unknown mode %s\n", argv[1]);
        return 1;
    }

    if(argc > 2)
    {
        passes = (uint32_t)strtoul(argv[2], NULL, 10);
        if(passes == 0U)
        {
            passes = 1U;
        }
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

    /* The DDL layer is started directly and the scheduler is left out, so that
     * nothing drives the servo except this test. */
    status = ddl_init(&test3_frame);
    if(status)
    {
        (void)fprintf(stderr, "Failed to initialize the DDL layer\n");
        return 1;
    }

    osal_delay_ms(TEST3_SETTLE_MS);

    if(run_scan)
    {
        test3_run_scan();
    }
    else
    {
        /* IDLE -> SCAN -> TARGET_LOCK, so that the FSM follows the target
         * registry instead of running its own pattern. */
        (void)test3_post_servo((uint32_t)eSERVO_EVENT_DIRECTIONS);
        osal_delay_ms(TEST3_SETTLE_MS);
        (void)test3_post_servo((uint32_t)eSERVO_EVENT_LOCK);
        osal_delay_ms(TEST3_SETTLE_MS);

        if(run_auto)
        {
            test3_run_auto(passes);
        }
        else if(run_manual)
        {
            test3_run_manual();
        }
        else
        {
            /* unreachable */
        }
    }

    (void)ddl_end();
    ddl_join();
    ddl_delete();
    util_event_bus_delete();
    hal_cleanup();
    log_exit();

    return 0;
}
