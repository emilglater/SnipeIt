#ifndef DDL_SERVO_EVENTS_H
#define DDL_SERVO_EVENTS_H

/* User library includes*/
#include "util/fsm/fsm.h"

typedef enum
{
    eSERVO_EVENT_SCAN = eFSM_EVENT_USER,
    eSERVO_EVENT_NOISE_DETECTED,
    eSERVO_EVENT_LOCK,
    eSERVO_EVENT_DIRECTIONS,
    eSERVO_EVENT_ROTATION_TIMEOUT,
    eSERVO_EVENT_TARGET_UPDATE
} eServoEvent;

#endif