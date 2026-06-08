#ifndef DDL_COMPASS_FSM_H
#define DDL_COMPASS_FSM_H

/* User library includes */
#include "util/fsm/fsm.h"

/**
 * @brief   The initial state of the magnetic sensor.
 * @details Verifies the sensor's Product ID over I2C and writes the
 *          desired bandwidth configuration. From this state we can go
 *          to compass_error and compass_idle states.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void compass_init_state(FSM* fsm, Event* event);

/**
 * @brief   The error state of the magnetic sensor.
 * @details This is a dead state - there is no option to go to
 *          another state from this state.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void compass_error_state(FSM* fsm, Event* event);

/**
 * @brief   The idle state of the magnetic sensor.
 * @details This state waits to receive a read request. From this
 *          state we can go to compass_read state.
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void compass_idle_state(FSM* fsm, Event* event);

/**
 * @brief   The read state of the magnetic sensor.
 * @details Performs the full SET / measure / RESET / measure / offset-
 *          removal / temperature read / heading compute sequence
 *          synchronously over I2C. Each measurement takes ~8 ms; the
 *          whole pass fits well inside one scheduler slot. From this
 *          state we go back to compass_idle (success) or to compass_idle
 *          with frame->valid = false (retries exhausted).
 * @param   fsm A pointer to an initialized FSM.
 * @param   event A pointer to an Event.
 */
void compass_read_state(FSM* fsm, Event* event);

#endif
