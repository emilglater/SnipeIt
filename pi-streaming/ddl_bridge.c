#include "ddl_bridge.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* src/ headers */
#include "util/event_bus/event_bus.h"
#include "util/event_bus/event_config.h"
#include "app/scheduler/scheduler_events.h"
#include "ddl/ddl_frame.h"
#include "util/log/log.h"
#include "hal/hal.h"
#include "app/app.h"

struct DdlBridge
{
    WebSocketServer* ws;
    unsigned int     period_ms;
    unsigned long    last_emit_ms;
    bool             hal_up;
    bool             bus_up;
    bool             app_up;
    bool             scheduler_started;
    bool             wind_offset_initialized;
    float            wind_sensor_zero_heading;
};

/* Monotonic clock — used for the period gate (immune to wall-clock changes). */
static unsigned long now_ms_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL +
           (unsigned long)(ts.tv_nsec / 1000000L);
}

/* Wall-clock ms since epoch — goes into the JSON "timestamp" field so the
 * app can correlate with its own clock. */
static unsigned long long now_ms_epoch(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000L);
}

static int build_json(const DDLFrame* f, const DdlBridge* b, char* out, size_t cap)
{
    const DistanceFrame*            d  = &f->dist_frame;
    const TemperatureHumidityFrame* th = &f->temp_hum_frame;
    const ServoFrame*               s  = &f->servo_frame;
    const GPSFrame*                 g  = &f->gps_frame;
    const MagFrame*                 c  = &f->mag_frame;
    const WindFrame*                w  = &f->wind_frame;

    float wind_direction_deg = 0.0f;
    if(b->wind_offset_initialized)
    {
        wind_direction_deg = fmodf(b->wind_sensor_zero_heading + w->direction_degrees, 360.0f);
        if(wind_direction_deg < 0.0f)
        {
            wind_direction_deg += 360.0f;
        }
    }
    else
    {
        wind_direction_deg = w->direction_degrees;
    }

    char heading_buf[32];
    if (c->valid && isfinite(c->heading_deg))
    {
        snprintf(heading_buf, sizeof(heading_buf), "%.2f", (double)c->heading_deg);
    }
    else
    {
        snprintf(heading_buf, sizeof(heading_buf), "null");
    }

    return snprintf(out, cap,
        "{"
            "\"type\":\"sensor_data\","
            "\"timestamp\":%llu,"
            "\"ddl_frame\":{"
                "\"distance\":{"
                    "\"valid\":%s,"
                    "\"distance_m\":%.2f,"
                    "\"status\":%u,"
                    "\"precision\":%u,"
                    "\"strength\":%u"
                "},"
                "\"temperature_humidity\":{"
                    "\"valid\":%s,"
                    "\"temperature_c\":%.2f,"
                    "\"humidity_pct\":%.2f"
                "},"
                "\"servo\":{"
                    "\"horizontal_deg\":%.2f,"
                    "\"vertical_deg\":%.2f"
                "},"
                "\"gps\":{"
                    "\"valid\":%s,"
                    "\"fix_type\":%u,"
                    "\"num_satellites\":%u,"
                    "\"latitude_deg\":%.7f,"
                    "\"longitude_deg\":%.7f,"
                    "\"altitude_m\":%.2f,"
                    "\"h_acc_m\":%.2f"
                "},"
                "\"compass\":{"
                    "\"valid\":%s,"
                    "\"raw_x\":%u,"
                    "\"raw_y\":%u,"
                    "\"raw_z\":%u,"
                    "\"temperature_c\":%.2f,"
                    "\"heading_deg\":%s"
                "},"
                "\"wind\":{"
                    "\"speed_valid\":%s,"
                    "\"speed_mps\":%.2f,"
                    "\"direction_valid\":%s,"
                    "\"direction_deg\":%.2f"
                "}"
            "}"
        "}",
        now_ms_epoch(),
        d->valid ? "true" : "false",
            (double)d->distance,
            (unsigned)d->status, (unsigned)d->precision, (unsigned)d->strength,
        th->valid ? "true" : "false",
            (double)th->temperature, (double)th->humidity,
        (double)s->hor_angle, (double)s->ver_angle,
        g->valid ? "true" : "false",
            (unsigned)g->fix_type, (unsigned)g->num_satellites,
            g->latitude, g->longitude,
            (double)g->altitude, (double)g->h_acc,
        c->valid ? "true" : "false",
            (unsigned)c->raw_x, (unsigned)c->raw_y, (unsigned)c->raw_z,
            (double)c->temperature_c, heading_buf,
        w->speed_valid ? "true" : "false", (double)w->speed,
        w->direction_valid ? "true" : "false", (double)wind_direction_deg);
}

static void calibrate_wind_offset(DdlBridge* b, const DDLFrame* snap)
{
    if(b->wind_offset_initialized)
    {
        return;
    }

    const MagFrame* c = &snap->mag_frame;
    const ServoFrame* s = &snap->servo_frame;

    if(!c->valid || !isfinite(c->heading_deg))
    {
        return;
    }

    /* Reject the reading unless the arm is at the calibration position.
     * Servos are open-loop on this rig, so hor_angle reflects the last
     * commanded angle; a tight tolerance is fine. */
    if(fabsf(s->hor_angle - 90.0f) > 2.0f)
    {
        return;
    }

    b->wind_sensor_zero_heading = c->heading_deg;
    b->wind_offset_initialized  = true;

    printf("[BRIDGE] Wind sensor 0-mark heading calibrated to %.2f deg "
           "(servo hor_angle %.2f)\n",
           (double)b->wind_sensor_zero_heading,
           (double)s->hor_angle);
}

DdlBridge* ddl_bridge_start(WebSocketServer* ws, unsigned int period_ms)
{
    if(ws == NULL)
    {
        return NULL;
    }

    DdlBridge* b = calloc(1, sizeof(*b));
    if(b == NULL)
    {
        return NULL;
    }
    b->ws = ws;
    b->period_ms = period_ms;

    if(log_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] log_init failed\n");
        free(b);
        return NULL;
    }

    if(hal_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] hal_init failed\n");
        log_exit();
        free(b);
        return NULL;
    }
    b->hal_up = true;

    if(util_event_bus_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] event bus init failed\n");
        ddl_bridge_stop(b);
        return NULL;
    }
    b->bus_up = true;

    if(app_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] app_init failed\n");
        ddl_bridge_stop(b);
        return NULL;
    }
    b->app_up = true;

    if(util_event_bus_publish(eAO_SCHEDULER, eSCHEDULER_EVENT_START) != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "[BRIDGE] failed to publish scheduler start\n");
        ddl_bridge_stop(b);
        return NULL;
    }
    b->scheduler_started = true;

    printf("[BRIDGE] Started — emitting sensor_data every %u ms\n", period_ms);
    return b;
}

void ddl_bridge_tick(DdlBridge* b)
{
    if(b == NULL || !b->scheduler_started)
    {
        return;
    }

    /* No client → nothing to do (skip JSON work entirely). */
    if(!ws_is_client_connected(b->ws))
    {
        return;
    }

    unsigned long t = now_ms_mono();
    if(b->last_emit_ms != 0 && (t - b->last_emit_ms) < b->period_ms)
    {
        return;
    }
    b->last_emit_ms = t;

    const DDLFrame* snap = app_get_ddl_snapshot();
    if(snap == NULL)
    {
        return;
    }

    calibrate_wind_offset(b, snap);

    char buf[1024];
    int n = build_json(snap, b, buf, sizeof(buf));
    if(n <= 0 || (size_t)n >= sizeof(buf))
    {
        fprintf(stderr, "[BRIDGE] JSON build failed (n=%d)\n", n);
        return;
    }

    if(ws_send_json(b->ws, buf, (size_t)n) != 0)
    {
        fprintf(stderr, "[BRIDGE] ws_send_json failed (queue full?)\n");
    }
}

void ddl_bridge_stop(DdlBridge* b)
{
    if(b == NULL)
    {
        return;
    }

    /* Tear down in reverse order, only what we actually brought up. */
    if(b->app_up)
    {
        (void)app_end();
        app_join();
        app_delete();
    }
    if(b->bus_up)
    {
        util_event_bus_delete();
    }
    if(b->hal_up)
    {
        hal_cleanup();
    }
    log_exit();
    free(b);
}