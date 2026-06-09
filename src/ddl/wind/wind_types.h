#ifndef DDL_WIND_TYPES_H
#define DDL_WIND_TYPES_H

/* Standard library includes */
#include <stdint.h>
#include <stdbool.h>

/* User library includes */
#include "util/active_object/active_object.h"
#include "wind_events.h"

typedef enum eWindTarget
{
    eWIND_TARGET_SPEED,
    eWIND_TARGET_DIRECTION
} eWindTarget;

typedef struct
{
    float   speed;              // in m/s
    float   direction_degrees;  // 0.0-359.9
    bool    speed_valid;
    bool    direction_valid;
    uint8_t reserved[2];
} WindFrame;

typedef struct
{
    ActiveObject   aobj;
    WindFrame*     frame;
    void*          timer;
    uint32_t       retry;
    eWindTarget    current_target;
} WindObject;

#endif