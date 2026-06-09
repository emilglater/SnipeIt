#ifndef DDL_WIND_EVENTS_H
#define DDL_WIND_EVENTS_H

/* User library includes */
#include "util/fsm/fsm.h"

typedef enum
{
    eWIND_EVENT_READ = eFSM_EVENT_USER,
    eWIND_EVENT_TIMEOUT,
    eWIND_EVENT_FRAME_RECEIVED
} eWindEvent;

#endif