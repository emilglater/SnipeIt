#ifndef DDL_WIND_FSM_H
#define DDL_WIND_FSM_H

/* User library includes */
#include "util/fsm/fsm.h"

/**
 * @brief   The initial state of the wind sensor module.
 * @details From this state we can go to wind_error and
 *          wind_idle states.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void wind_init_state(FSM* fsm, Event* event);

/**
 * @brief   The error state of the wind sensor module.
 * @details This is a dead state - there is no option to go to
 *          another state from this state.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void wind_error_state(FSM* fsm, Event* event);

/**
 * @brief   The idle state of the wind sensor module.
 * @details This state waits to receive a read request. From this
 *          state we can go to wind_read state. On entry it resets
 *          the retry counter and sets the current target to SPEED,
 *          so each new read cycle begins with the speed sensor.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void wind_idle_state(FSM* fsm, Event* event);

/**
 * @brief   The read state of the wind sensor module.
 * @details This state issues a Modbus read query for the sensor
 *          currently selected by current_target and waits for the
 *          response (timer-bounded). From this state we can go to
 *          wind_update state.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void wind_read_state(FSM* fsm, Event* event);

/**
 * @brief   The update state of the wind sensor module.
 * @details Validates the response and writes the relevant half of
 *          the frame. If the speed read succeeded, advances target
 *          to direction and re-enters wind_read. If the direction
 *          read succeeded, returns to wind_idle. On retry-exhausted
 *          failure the relevant validity flag is cleared but the
 *          cycle continues so one dead sensor does not block the other.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void wind_update_state(FSM* fsm, Event* event);

#endif