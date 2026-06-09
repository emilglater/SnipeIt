#ifndef DDL_FRAME_H
#define DDL_FRAME_H

/* Standard library includes */
#include <stdint.h>

/* User library includes */
#include "ddl/temperature_humidity/temperature_humidity_types.h"
#include "ddl/distance/distance_types.h"
#include "ddl/compass/compass_types.h"
#include "ddl/servo/servo_types.h"
#include "ddl/wind/wind_types.h"
#include "ddl/gps/gps_types.h"

// This struct will contain a field for every sensor whose
// data we want to transfer to the Android application
typedef struct
{
    GPSFrame                    gps_frame;      // size 32 bytes; alignment 8 bytes
    ServoFrame                  servo_frame;    // size 8 bytes; alignment 4 bytes
    MagFrame                    mag_frame;      // size 24 bytes; alignment 4 bytes
    DistanceFrame               dist_frame;     // size 12 bytes; alignment 4 bytes
    uint32_t                    reserved1;
    TemperatureHumidityFrame    temp_hum_frame; // size 12 bytes; alignment 4 bytes
    WindFrame                   wind_frame;     // size 12 bytes; alignment 4 bytes
    uint64_t                    resevred2[2];
} DDLFrame;

#endif