#include "broadcaster_fsm.h"

/* User library includes */
#include "app/broadcaster/broadcaster_types.h"
#include "util/log/log.h"

static void copy_frame(BroadcasterObject* aobj)
{
    DDLFrame* src = aobj->source;
    DDLFrame* dst = aobj->destination;

    /* Distance */
    dst->dist_frame.valid     = src->dist_frame.valid;
    dst->dist_frame.distance  = src->dist_frame.distance;
    dst->dist_frame.status    = src->dist_frame.status;
    dst->dist_frame.precision = src->dist_frame.precision;
    dst->dist_frame.strength  = src->dist_frame.strength;

    /* Temperature / Humidity */
    dst->temp_hum_frame.valid       = src->temp_hum_frame.valid;
    dst->temp_hum_frame.humidity    = src->temp_hum_frame.humidity;
    dst->temp_hum_frame.temperature = src->temp_hum_frame.temperature;

    /* Servo */
    dst->servo_frame.hor_angle = src->servo_frame.hor_angle;
    dst->servo_frame.ver_angle = src->servo_frame.ver_angle;

    /* GPS */
    dst->gps_frame.latitude       = src->gps_frame.latitude;
    dst->gps_frame.longitude      = src->gps_frame.longitude;
    dst->gps_frame.altitude       = src->gps_frame.altitude;
    dst->gps_frame.h_acc          = src->gps_frame.h_acc;
    dst->gps_frame.valid          = src->gps_frame.valid;
    dst->gps_frame.fix_type       = src->gps_frame.fix_type;
    dst->gps_frame.num_satellites = src->gps_frame.num_satellites;

    /* Compass */
    dst->mag_frame.valid            = src->mag_frame.valid;
    dst->mag_frame.heading_deg      = src->mag_frame.heading_deg;
    dst->mag_frame.raw_x            = src->mag_frame.raw_x;
    dst->mag_frame.raw_y            = src->mag_frame.raw_y;
    dst->mag_frame.raw_z            = src->mag_frame.raw_z;
    dst->mag_frame.temperature_c    = src->mag_frame.temperature_c;

    /* Wind */
    dst->wind_frame.speed_valid         = src->wind_frame.speed_valid;
    dst->wind_frame.direction_valid     = src->wind_frame.direction_valid;
    dst->wind_frame.speed               = src->wind_frame.speed;
    dst->wind_frame.direction_degrees   = src->wind_frame.direction_degrees;
}

void broadcaster_init_state(FSM* fsm, Event* event)
{
    switch(event->type)
    {
    case eFSM_EVENT_INIT:
        LOG_DEBUG("INIT entry");
        (void)util_fsm_transition(fsm, broadcaster_idle_state);
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("INIT exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void broadcaster_idle_state(FSM* fsm, Event* event)
{
    BroadcasterObject* aobj = (BroadcasterObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("IDLE entry");
        break;
    case eBROADCASTER_EVENT_UPDATE:
        LOG_DEBUG("Updating DDLFrame broadcaster");
        copy_frame(aobj);
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("IDLE exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}