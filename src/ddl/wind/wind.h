#ifndef DDL_WIND_H
#define DDL_WIND_H

/* User library includes */
#include "ddl/ddl_frame.h"
#include "util/fsm/fsm.h"
#include "status.h"

/**
 * @brief   Initialize the wind sensor module.
 * @details Drives both the wind speed and wind direction sensors over a shared
 *          RS485 bus.
 * @param   frame A pointer to the DDLFrame that will hold all our sensor data.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      frame is NULL or the WindObject is misconfigured
 * @retval  eSTATUS_SYSTEM_ERROR    thread or queue initialization failed
 */
eStatus ddl_wind_init(DDLFrame* frame);

/**
 * @brief   Send event to the wind sensor module.
 * @param   event An event from @ref eWindEvent.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      event is NULL or sensor is uninitialized
 * @retval  eSTATUS_ACTION_FAILED   thread or queue action failed
 */
eStatus ddl_wind_post(Event* event);

/**
 * @brief   Move to the END state of the wind sensor module.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      the WindObject is misconfigured
 * @retval  eSTATUS_ACTION_FAILED   thread or queue action failed
 */
eStatus ddl_wind_end(void);

/**
 * @brief   Wait for the wind sensor module to stop.
 */
void ddl_wind_join(void);

/**
 * @brief   Free the wind sensor module's resources.
 */
void ddl_wind_delete(void);

#endif