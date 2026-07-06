#ifndef DDL_SERVO_H
#define DDL_SERVO_H

/* User library includes */
#include "ddl/ddl_frame.h"
#include "util/fsm/fsm.h"
#include "status.h"

/**
 * @brief   Initialize the PCA9685 PWM driver (servo controller).
 * @param   frame A pointer to the DDLFrame that will hold all our angle data.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      frame is NULL or the ServoObject is misconfigured
 * @retval  eSTATUS_SYSTEM_ERROR    thread or queue initalization failed
 */
eStatus ddl_servo_init(DDLFrame* frame);

/**
 * @brief   Send event to the servo motors.
 * @param   event An event from @ref eServoEvent.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      event is NULL or servos are uninitialized
 * @retval  eSTATUS_ACTION_FAILED   thread or queue action failed
 */
eStatus ddl_servo_post(Event* event);

/**
 * @brief   Move to the END state of the servo motors.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      the ServoObject is misconfigured
 * @retval  eSTATUS_ACTION_FAILED   thread or queue action failed
 */
eStatus ddl_servo_end(void);

/**
 * @brief   Wait for the servo motors to stop.
 */
void ddl_servo_join(void);

/**
 * @brief   Free the servo motors' resources.
 */
void ddl_servo_delete(void);

/**
 * @brief   Update the target angles registry that the servo FSM
 *          uses on lock and noise-scan events.
 * @details Setting the target is seperate from publishing the event.
 * @param   hor_angle Target horizontal angle, in degrees.
 * @param   ver_angle Target vertical angle, in degrees.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_INVALID_VALUE   angle outside the legal range
 * @retval  eSTATUS_ACTION_FAILED   mutex not initialized
 */
eStatus ddl_servo_set_target(float hor_angle, float ver_angle);

/**
 * @brief   Read the last angles actually commanded to the servos.
 * @details Always fresh (updated the instant the FSM drives the PWM
 *          controller), unlike the broadcaster snapshot which is refreshed
 *          only once per scheduler cycle. Use for capture-time pose stamping.
 * @param   hor_angle Out: last commanded horizontal angle, in degrees.
 * @param   ver_angle Out: last commanded vertical angle, in degrees.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      an out pointer is NULL
 * @retval  eSTATUS_ACTION_FAILED   servo FSM not started yet
 */
eStatus ddl_servo_get_pose(float* hor_angle, float* ver_angle);

#endif