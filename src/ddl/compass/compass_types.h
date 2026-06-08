#ifndef DDL_COMPASS_TYPES_H
#define DDL_COMPASS_TYPES_H

/* Standard library includes */
#include <stdint.h>
#include <stdbool.h>

/* User library includes */
#include "util/active_object/active_object.h"
#include "compass_events.h"

typedef struct
{
    bool        valid;
    uint8_t     reserved[3];
    uint32_t    raw_x;
    uint32_t    raw_y;
    uint32_t    raw_z;
    float       temperature_c;
    float       heading_deg;
} MagFrame;

typedef struct
{
    ActiveObject    aobj;
    MagFrame*       frame;
    uint32_t        retry;
    uint32_t        reserved;
} CompassObject;


#endif