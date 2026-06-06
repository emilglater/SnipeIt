#ifndef HAL_UART_H
#define HAL_UART_H

/* Standard library includes */
#include <stdint.h>
#include <stddef.h>

/* User library includes */
#include "status.h"

typedef void (*async_cb)(void *user_data);

/**
 * @brief   Set the UART devices configuration.
 * @details Sets the UART devices according to configuration specified
 *          in 'hal_uart_config.h', and opens them for communication.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_DEVICE_ERROR    failure to set the device configuration
 * @retval  eSTATUS_SYSTEM_ERROR    io-uring initialization failed
 */
eStatus hal_uart_init(void);

/**
 * @brief   Write to device in non-blocking mode.
 * @details Writes to device using io-uring.
 * @param   device_index A value from @ref eUARTDeviceNumber.
 * @param   buffer A pointer to the transmit buffer.
 * @param   len The number of bytes to write to the device.
 * @param   callback The function that should be called upon write completeion.
 *          Note that this function cannot call another UART operation from within
 *          or a deadlock may occur.
 * @param   arg Optional argument that can be used by callback.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_INVALID_VALUE   device_index is not from @ref eUARTDeviceNumber
 * @retval  eSTATUS_NULL_PARAM      buffer is NULL
 * @retval  eSTATUS_ACTION_FAILED   couldn't allocate an operation slot
 * @retval  eSTATUS_SYSTEM_ERROR    couldn't get submition queue or submit operation
 */
eStatus hal_uart_write(uint32_t device_index, const void* buffer, uint32_t len, async_cb callback, void* arg);

/**
 * @brief   Read from device in non-blocking mode.
 * @details Reads from device using io-uring.
 * @param   device_index A value from @ref eUARTDeviceNumber.
 * @param   buffer A pointer to the transmit buffer.
 * @param   len The number of bytes to read from the device.
 * @param   callback The function that should be called upon read completeion.
 *          Note that this function cannot call another UART operation from within
 *          or a deadlock may occur.
 * @param   arg Optional argument that can be used by callback.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_INVALID_VALUE   device_index is not from @ref eUARTDeviceNumber
 * @retval  eSTATUS_NULL_PARAM      buffer is NULL
 * @retval  eSTATUS_ACTION_FAILED   couldn't allocate an operation slot
 * @retval  eSTATUS_SYSTEM_ERROR    couldn't get submition queue or submit operation
 */
eStatus hal_uart_read(uint32_t device_index, void* buffer, uint32_t len, async_cb callback, void* arg);

/**
 * @brief   Flush the input buffer.
 * @param   device_index A value from @ref eUARTDeviceNumber.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_INVALID_VALUE   device_index is not from @ref eUARTDeviceNumber
 */
eStatus hal_uart_flush_input(uint32_t device_index);

/**
 * @brief   Abort submitted device operation.
 * @param   device_index A value from @ref eUARTDeviceNumber.
 * @returns A value from @ref eStatus.
 * @retval  eSTATUS_SUCCESSFUL      successful execution
 * @retval  eSTATUS_INVALID_VALUE   device_index is not from @ref eUARTDeviceNumber
 * @retval  eSTATUS_SYSTEM_ERROR    couldn't get submition queue or submit operation
 */
eStatus hal_uart_abort(uint32_t device_index);

/**
 * @brief   Release UART resources.
 * @details Stops the io-uring thread, closes the queue, and closes the devices' filedescriptors.
 */
void hal_uart_cleanup(void);

#endif