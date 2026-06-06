#ifndef HAL_UART_CONFIG_H
#define HAL_UART_CONFIG_H

typedef enum eUARTBaud
{
    eBAUD0,
    eBAUD50,
    eBAUD75,
    eBAUD110,
    eBAUD134,
    eBAUD150,
    eBAUD200,
    eBAUD300,
    eBAUD600,
    eBAUD1200,
    eBAUD1800,
    eBAUD2400,
    eBAUD4800,
    eBAUD9600,
    eBAUD19200,
    eBAUD38400,
    eBAUD57600,
    eBAUD115200,
    eBAUD230400,
    eBAUD460800
} eUARTBaud;

typedef enum eUARTBitsPerByte
{
    e5BITS_PER_BYTE,
    e6BITS_PER_BYTE,
    e7BITS_PER_BYTE,
    e8BITS_PER_BYTE
} eUARTBitsPerByte;

typedef enum eUARTStopBit
{
    eSINGLE_STOP_BIT,
    eDOUBLE_STOP_BIT
} eUARTStopBit;

typedef enum eUARTParityBit
{
    eNO_PARITY_BIT,
    eODD_PARITY_BIT,
    eEVEN_PARITY_BIT
} eUARTParityBit;

// This numeration specifies which device should be addressed.
typedef enum eUARTDeviceNumber
{
    eUART0_DEVICE,
    eUART1_DEVICE,
    eUART2_DEVICE,
    eUART_DEVICE_COUNT
} eUARTDeviceNumber;

typedef enum eUARTConfig
{
    eUART_MAX_QUEUED_OPERATIONS = 4,

    eUART0_BAUD_CONFIG           = eBAUD9600,
    eUART0_BITS_PER_BYTE_CONFIG  = e8BITS_PER_BYTE,
    eUART0_STOP_BIT_CONFIG       = eSINGLE_STOP_BIT,
    eUART0_PARITY_BIT_CONFIG     = eNO_PARITY_BIT,
    

    eUART1_BAUD_CONFIG           = eBAUD38400,
    eUART1_BITS_PER_BYTE_CONFIG  = e8BITS_PER_BYTE,
    eUART1_STOP_BIT_CONFIG       = eSINGLE_STOP_BIT,
    eUART1_PARITY_BIT_CONFIG     = eNO_PARITY_BIT,

    eUART2_BAUD_CONFIG           = eBAUD9600,
    eUART2_BITS_PER_BYTE_CONFIG  = e8BITS_PER_BYTE,
    eUART2_STOP_BIT_CONFIG       = eSINGLE_STOP_BIT,
    eUART2_PARITY_BIT_CONFIG     = eNO_PARITY_BIT,
} eUARTConfig;

#endif