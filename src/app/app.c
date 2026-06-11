#include "app.h"

/* User library includes */
#include "broadcaster/broadcaster_events.h"
#include "util/event_bus/event_config.h"
#include "scheduler/scheduler_events.h"
#include "util/event_bus/event_bus.h"
#include "broadcaster/broadcaster.h"
#include "scheduler/scheduler.h"
#include "util/log/log.h"
#include "ddl/ddl.h"

typedef struct
{
    eStatus (*module_init)(void);
    eStatus (*module_post)(Event* event);
    eStatus (*module_end)(void);
    void (*module_join)(void);
    void (*module_delete)(void);
    uint32_t    ao_id;
    Event       subscribe_event;
    const char* module_name;
} APPModule;

static APPModule app_modules[eAPP_MODULE_COUNT] = {
    [eAPP_MODULE_SCHEDULER] = {
        .module_init        = app_scheduler_init,
        .module_post        = app_scheduler_post,
        .module_end         = app_scheduler_end,
        .module_join        = app_scheduler_join,
        .module_delete      = app_scheduler_delete,
        .ao_id              = eAO_SCHEDULER,
        .subscribe_event    = { .type = eSCHEDULER_EVENT_START },
        .module_name        = "scheduler"
    },
    [eAPP_MODULE_BROADCASTER] = {
        .module_init        = app_broadcaster_init,
        .module_post        = app_broadcaster_post,
        .module_end         = app_broadcaster_end,
        .module_join        = app_broadcaster_join,
        .module_delete      = app_broadcaster_delete,
        .ao_id              = eAO_BROADCASTER,
        .subscribe_event    = { .type = eBROADCASTER_EVENT_UPDATE },
        .module_name        = "broadcaster"
    }
};

static DDLFrame ddl_frame;
static DDLFrame ddl_snapshot;

eStatus app_init(void)
{
    LOG_INFO("Initializing the DDL layer");
    eStatus status = ddl_init(&ddl_frame);
    if(status)
    {
        LOG_ERROR("Failed to initialize the DDL layer");
        return status;
    }

    LOG_DEBUG("Configuring the broadcaster module");
    status = app_broadcaster_configure(&ddl_frame, &ddl_snapshot);
    if(status)
    {
        LOG_ERROR("Failed to configure the broadcaster module");
        return status;
    }

    LOG_INFO("Initializing the APP layer");
    for(uint32_t module_index = 0; module_index < eAPP_MODULE_COUNT; module_index++)
    {
        LOG_DEBUG("Initializing %s module", app_modules[module_index].module_name);
        status = app_modules[module_index].module_init();
        if(status)
        {
            return status;
        }
        status = util_event_bus_subscribe(app_modules[module_index].ao_id, app_post,
                                            module_index, &app_modules[module_index].subscribe_event);
        if(status)
        {
            return status;
        }
    }

    LOG_DEBUG("Registering modules to scheduler");
    static Event gps_read_event = { .type = eGPS_EVENT_READ };
    static Event wind_read_event = { .type = eWIND_EVENT_READ };
    static Event compass_read_event = { .type = eCOMPASS_EVENT_READ };
    static Event distance_read_event = { .type = eDISTANCE_EVENT_READ };
    static Event servo_directions_event = { .type = eSERVO_EVENT_DIRECTIONS };
    static Event broadcaster_update_event = { .type = eBROADCASTER_EVENT_UPDATE };
    static Event temperature_humidity_read_event = { .type = eTEMPERATURE_HUMIDITY_EVENT_READ };
    // TODO: hardcoded, refactor later
    // Can add a `ddl_subscribe()`-type of function that will go over the modules and
    // call `app_scheduler_subscribe()` for each
    status = app_scheduler_subscribe(0, eAO_COMPASS, &compass_read_event);
    if(status)
    {
        return status;
    }
    status = app_scheduler_subscribe(1, eAO_DISTANCE, &distance_read_event);
    if(status)
    {
        return status;
    }
    status = app_scheduler_subscribe(2, eAO_TEMPERATURE_HUMIDITY, &temperature_humidity_read_event);
    if(status)
    {
        return status;
    }
    status = app_scheduler_subscribe(3, eAO_SERVO, &servo_directions_event);
    if(status)
    {
        return status;
    }
    status = app_scheduler_subscribe(4, eAO_WIND, &wind_read_event);
    if(status)
    {
        return status;
    }
    status = app_scheduler_subscribe(5, eAO_GPS, &gps_read_event);
    if(status)
    {
        return status;
    }
    status = app_scheduler_subscribe(6, eAO_BROADCASTER, &broadcaster_update_event);

    return status;
}

eStatus app_post(uint32_t module, Event* event)
{
    if(module >= eAPP_MODULE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }

    LOG_DEBUG("Event posted to %s module", app_modules[module].module_name);
    return app_modules[module].module_post(event);
}

eStatus app_end(void)
{
    eStatus status;

    LOG_INFO("Ending the APP layer");
    for(int module_index = 0; module_index < eAPP_MODULE_COUNT; module_index++)
    {
        LOG_DEBUG("End event sent to %s module", app_modules[module_index].module_name);
        status = app_modules[module_index].module_end();
        if(status)
        {
            return status;
        }
    }

    LOG_INFO("Ending the DDL layer");
    status = ddl_end();
    if(status)
    {
        LOG_ERROR("Failed to end DDL layer with status %d", status);
        return status;
    }

    return eSTATUS_SUCCESSFUL;
}

void app_join(void)
{
    LOG_INFO("Joining APP threads");
    for(int module_index = 0; module_index < eAPP_MODULE_COUNT; module_index++)
    {
        LOG_DEBUG("Joining %s thread", app_modules[module_index].module_name);
        app_modules[module_index].module_join();
    }

    LOG_INFO("Joining DDL threads");
    ddl_join();
}

void app_delete(void)
{
    LOG_INFO("Deleting APP resources");
    for(int module_index = 0; module_index < eAPP_MODULE_COUNT; module_index++)
    {
        LOG_DEBUG("Delete %s resources", app_modules[module_index].module_name);
        app_modules[module_index].module_delete();
    }

    LOG_INFO("Deleting DDL resources");
    ddl_delete();
}

const DDLFrame* app_get_ddl_snapshot(void)
{
    return &ddl_snapshot;
}