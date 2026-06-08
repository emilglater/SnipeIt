#ifndef DDL_COMPASS_H
#define DDL_COMPASS_H

/* User library includes */
#include "ddl/ddl_frame.h"
#include "util/fsm/fsm.h"
#include "status.h"

/**
 * @brief   Initialize the MMC5983MA magnetic sensor.
 * @param   frame A pointer to the DDLFrame that will hold all our sensor data.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      frame is NULL or the CompassObject is misconfigured
 * @retval  eSTATUS_SYSTEM_ERROR    thread or queue initalization failed
 */
eStatus ddl_compass_init(DDLFrame* frame);

/**
 * @brief   Send event to the magnetic sensor.
 * @param   event An event from @ref eCompassEvent.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      event is NULL or sensor is uninitialized
 * @retval  eSTATUS_ACTION_FAILED   thread or queue action failed
 */
eStatus ddl_compass_post(Event* event);

/**
 * @brief   Move to the END state of the magnetic sensor.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      the CompassObject is misconfigured
 * @retval  eSTATUS_ACTION_FAILED   thread or queue action failed
 */
eStatus ddl_compass_end(void);

/**
 * @brief   Wait for the magnetic sensor to stop.
 */
void ddl_compass_join(void);

/**
 * @brief   Free the magnetic sensor's resources.
 */
void ddl_compass_delete(void);

#endif