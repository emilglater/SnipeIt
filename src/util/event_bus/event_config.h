#ifndef UTIL_EVENT_CONFIG_H
#define UTIL_EVENT_CONFIG_H

/**
 * @brief   Active Object module identifiers.
 * @details Each Active Object module in the system must have a
 *          unique identifier listed here. Used by the event bus
 *          for routing events to the correct subscriber.
 */
typedef enum eActiveObjectID
{
    eAO_SCHEDULER,
    eAO_DISTANCE,
    eAO_SERVO,
    eAO_TEMPERATURE_HUMIDITY,
    eAO_GPS,
    eAO_BROADCASTER,
    eAO_COMPASS,
    eAO_COUNT
} eActiveObjectID;

#endif
