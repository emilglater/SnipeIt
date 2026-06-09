#ifndef DDL_WIND_CONFIG_H
#define DDL_WIND_CONFIG_H

/* User library includes */
#include "hal/uart/hal_uart_config.h"

typedef enum eWindConfig
{
    eWIND_QUEUE_CAPACITY      = 4,
    eWIND_READ_RETRY_MAX      = 3,
    eWIND_READ_TIMEOUT_MS     = 200,
    eWIND_UART_DEVICE         = eUART2_DEVICE,
    eWIND_SPEED_ADDRESS       = 0x01,           // Modbus address
    eWIND_DIRECTION_ADDRESS   = 0x02            // Modbus address
} eWindConfig;

#endif