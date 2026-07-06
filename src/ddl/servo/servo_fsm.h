#ifndef DDL_SERVO_FSM_H
#define DDL_SERVO_FSM_H

/* User library includes */
#include "util/fsm/fsm.h"

/**
 * @brief   The initial state of the servo motors.
 * @details From this state we can go to servo_error and
 *          servo_idle states.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void servo_init_state(FSM* fsm, Event* event);

/**
 * @brief   The error state of the servo motors.
 * @details This is a dead state - there is no option to go to
 *          another state from this state.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void servo_error_state(FSM* fsm, Event* event);

/**
 * @brief   The idle state of the servo motors.
 * @details From this state we can go to servo_error and
 *          servo_scan states.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void servo_idle_state(FSM* fsm, Event* event);

/**
 * @brief   The scan state of the servo motors.
 * @details From this state we can go to servo_idle,
 *          servo_noise_sacn and servo_target_lock states.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void servo_scan_state(FSM* fsm, Event* event);

/**
 * @brief   The noise scan state of the servo motors.
 * @details From this state we can go to servo_scan state.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void servo_noise_scan_state(FSM* fsm, Event* event);

/**
 * @brief   The target lock state of the servo motors.
 * @details From this state we can go to servo_scan state.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void servo_target_lock_state(FSM* fsm, Event* event);

/**
 * @brief   Atomically update the servo target registry.
 * @details Setting the target is seperate from publishing the event.
 * @param   hor_angle Target horizontal angle, in degrees.
 * @param   ver_angle Target vertical angle, in degrees.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_INVALID_VALUE   angle outside the legal range
 * @retval  eSTATUS_ACTION_FAILED   mutex not initialized
 */
eStatus servo_fsm_set_target(float hor_angle, float ver_angle);

/**
 * @brief   Read the last angles actually commanded to the servos.
 * @details Updated the instant the FSM writes the PWM controller, so it is
 *          always fresh (the broadcaster snapshot can be a full scheduler
 *          cycle stale). Intended for capture-time pose stamping.
 * @param   hor_angle Out: last commanded horizontal angle, in degrees.
 * @param   ver_angle Out: last commanded vertical angle, in degrees.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_NULL_PARAM      an out pointer is NULL
 * @retval  eSTATUS_ACTION_FAILED   mutex not initialized (FSM not started)
 */
eStatus servo_fsm_get_pose(float* hor_angle, float* ver_angle);

/**
 * @brief   Releases the servo's FSM resources.
 * @details For now it only destroys a mutex used by the FSM.
 */
void servo_fsm_destroy();

#endif