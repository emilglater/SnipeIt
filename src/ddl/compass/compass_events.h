#ifndef DDL_COMPASS_EVENTS_H
#define DDL_COMPASS_EVENTS_H

/* User library includes*/
#include "util/fsm/fsm.h"

typedef enum
{
    eCOMPASS_EVENT_READ = eFSM_EVENT_USER
} eCompassEvent;

#endif