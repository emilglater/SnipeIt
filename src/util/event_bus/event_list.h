#ifndef UTIL_EVENT_LIST_H
#define UTIL_EVENT_LIST_H

/*
    Central event type registry.
    Includes the event enums of every Active Object module.
    When adding a new AO module, include its x_events.h here.
*/

#include "ddl/temperature_humidity/temperature_humidity_events.h"
#include "app/broadcaster/broadcaster_events.h"
#include "app/scheduler/scheduler_events.h"
#include "ddl/distance/distance_events.h"
#include "ddl/compass/compass_events.h"
#include "ddl/servo/servo_events.h"
#include "ddl/gps/gps_events.h"
// Add new module event headers here

#endif
