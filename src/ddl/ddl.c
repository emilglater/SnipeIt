#include "ddl.h"

/* User library includes */
#include "ddl/temperature_humidity/temperature_humidity.h"
#include "util/event_bus/event_config.h"
#include "util/event_bus/event_bus.h"
#include "ddl/distance/distance.h"
#include "ddl/compass/compass.h"
#include "ddl/servo/servo.h"
#include "ddl/wind/wind.h"
#include "util/log/log.h"
#include "ddl/gps/gps.h"

typedef struct
{
    eStatus (*module_init)(DDLFrame* frame);
    eStatus (*module_post)(Event* event);
    eStatus (*module_end)(void);
    void (*module_join)(void);
    void (*module_delete)(void);
    uint32_t    ao_id;
    uint32_t    subscribe_events_count;
    Event*      subscribe_events;
    const char* module_name;
} DDLModule;

static Event distance_subscribe_events[] = {
    { . type = eDISTANCE_EVENT_READ }
};

static Event servo_subscribe_events[] = {
    { .type = eSERVO_EVENT_SCAN },
    { .type = eSERVO_EVENT_DIRECTIONS },
    { .type = eSERVO_EVENT_NOISE_DETECTED },
    { .type = eSERVO_EVENT_LOCK },
    { .type = eSERVO_EVENT_TARGET_UPDATE }
};

static Event temperature_humidity_subscribe_events[] = {
    { .type = eTEMPERATURE_HUMIDITY_EVENT_READ }
};

static Event gps_subscribe_events[] = {
    { .type = eGPS_EVENT_READ }
};

static Event compass_subscribe_events[] = {
    { .type = eCOMPASS_EVENT_READ }
};

static Event wind_subscribe_events[] = {
    { .type = eWIND_EVENT_READ }
};

static DDLModule ddl_modules[eDLL_MODULE_COUNT] = {
    [eDDL_MODULE_DISTANCE] = {
        .module_init            = ddl_distance_init,
        .module_post            = ddl_distance_post,
        .module_end             = ddl_distance_end,
        .module_join            = ddl_distance_join,
        .module_delete          = ddl_distance_delete,
        .ao_id                  = eAO_DISTANCE,
        .subscribe_events_count = sizeof(distance_subscribe_events) / 
                                    sizeof(distance_subscribe_events[0]),
        .subscribe_events       = distance_subscribe_events,
        .module_name            = "distance"
    },
    [eDDL_MODULE_SERVO] = {
        .module_init            = ddl_servo_init,
        .module_post            = ddl_servo_post,
        .module_end             = ddl_servo_end,
        .module_join            = ddl_servo_join,
        .module_delete          = ddl_servo_delete,
        .ao_id                  = eAO_SERVO,
        .subscribe_events_count = sizeof(servo_subscribe_events) /
                                    sizeof(servo_subscribe_events[0]),
        .subscribe_events       = servo_subscribe_events,
        .module_name            = "servo"
    },
    [eDDL_MODULE_TEMPERATURE_HUMIDITY] = {
        .module_init            = ddl_temperature_humidity_init,
        .module_post            = ddl_temperature_humidity_post,
        .module_end             = ddl_temperature_humidity_end,
        .module_join            = ddl_temperature_humidity_join,
        .module_delete          = ddl_temperature_humidity_delete,
        .ao_id                  = eAO_TEMPERATURE_HUMIDITY,
        .subscribe_events_count = sizeof(temperature_humidity_subscribe_events) / 
                                    sizeof(temperature_humidity_subscribe_events[0]),
        .subscribe_events       = temperature_humidity_subscribe_events,
        .module_name            = "temperature-humidity"
    },
    [eDDL_MODULE_GPS] = {
        .module_init            = ddl_gps_init,
        .module_post            = ddl_gps_post,
        .module_end             = ddl_gps_end,
        .module_join            = ddl_gps_join,
        .module_delete          = ddl_gps_delete,
        .ao_id                  = eAO_GPS,
        .subscribe_events_count = sizeof(gps_subscribe_events) / 
                                    sizeof(gps_subscribe_events[0]),
        .subscribe_events       = gps_subscribe_events,
        .module_name            = "gps"
    },
    [eDDL_MODULE_COMPASS] = {
        .module_init            = ddl_compass_init,
        .module_post            = ddl_compass_post,
        .module_end             = ddl_compass_end,
        .module_join            = ddl_compass_join,
        .module_delete          = ddl_compass_delete,
        .ao_id                  = eAO_COMPASS,
        .subscribe_events_count = sizeof(compass_subscribe_events) /
                                    sizeof(compass_subscribe_events[0]),
        .subscribe_events       = compass_subscribe_events,
        .module_name            = "compass"
    },
    [eDDL_MODULE_WIND] = {
        .module_init            = ddl_wind_init,
        .module_post            = ddl_wind_post,
        .module_end             = ddl_wind_end,
        .module_join            = ddl_wind_join,
        .module_delete          = ddl_wind_delete,
        .ao_id                  = eAO_WIND,
        .subscribe_events_count = sizeof(wind_subscribe_events) /
                                    sizeof(wind_subscribe_events[0]),
        .subscribe_events       = wind_subscribe_events,
        .module_name            = "wind"
    }
};

eStatus ddl_init(DDLFrame* frame)
{
    for(uint32_t module_index = 0; module_index < eDLL_MODULE_COUNT; module_index++)
    {
        DDLModule* module = &ddl_modules[module_index];
        LOG_DEBUG("Initializing %s module", module->module_name);
        eStatus status = module->module_init(frame);
        if(status)
        {
            return status;
        }
        for(uint32_t i = 0; i < module->subscribe_events_count; i++)
        {
            status = util_event_bus_subscribe(module->ao_id, ddl_post, module_index,
                                                &module->subscribe_events[i]);
            if(status)
            {
                return status;
            }
        }
    }

    return eSTATUS_SUCCESSFUL;
}

eStatus ddl_post(uint32_t module, Event* event)
{
    if(module >= eDLL_MODULE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }

    LOG_DEBUG("Event posted to %s module", ddl_modules[module].module_name);
    return ddl_modules[module].module_post(event);
}

eStatus ddl_end(void)
{
    for(int module_index = 0; module_index < eDLL_MODULE_COUNT; module_index++)
    {
        LOG_DEBUG("End event sent to %s module", ddl_modules[module_index].module_name);
        eStatus status = ddl_modules[module_index].module_end();
        if(status)
        {
            return status;
        }
    }

    return eSTATUS_SUCCESSFUL;
}

void ddl_join(void)
{
    for(int module_index = 0; module_index < eDLL_MODULE_COUNT; module_index++)
    {
        LOG_DEBUG("Joining %s thread", ddl_modules[module_index].module_name);
        ddl_modules[module_index].module_join();
    }
}

void ddl_delete(void)
{
    for(int module_index = 0; module_index < eDLL_MODULE_COUNT; module_index++)
    {
        LOG_DEBUG("Delete %s resources", ddl_modules[module_index].module_name);
        ddl_modules[module_index].module_delete();
    }
}