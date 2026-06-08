#include "hal_i2c.h"

/* Standard Libraries */
#include <stddef.h>
#include <string.h>

/* Linux Specific Libraries */
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>      // The main communication is implemented using the `ioctl()` function
#include <unistd.h>
#include <fcntl.h>

/* User Libraries */
#include "hal_i2c_config.h"

#define I2C_SINGLE_MESSAGE 1
#define I2C_DOUBLE_MESSAGE 2

#define I2C_MAX_MESSAGE_SIZE_BYTES 8

typedef struct
{
    char*       path;       /** Path to the I2C bus, e.g. "/dev/i2c-1" */
    int         fd;         /** File descriptor for the I2C device */
    uint16_t    flags;      /** Flags determining the functionality of the messages handled by the device  */
    uint8_t     address;    /** Slave address used to for communication */
    uint8_t     padding;
} I2CDevice;

static I2CDevice i2c_devices[eI2C_DEVICE_COUNT] = {
    [eI2C0_DEVICE] = {
        .path = "/dev/i2c-1",
        .fd = -1,
        .flags = 0,      // Possible flag is `I2C_M_TEN` for 10bit address length (defualt is 7bit)
        .address = 0
    },
    [eI2C1_DEVICE] = {
        .path = "/dev/i2c-1",
        .fd = -1,
        .flags = 0,
        .address = 0
    }
};

static eStatus hal_i2c_transfer(uint32_t device_index, struct i2c_msg* messages, size_t count)
{
    if(device_index >= eI2C_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }
    if(messages == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }
    if(count > I2C_RDWR_IOCTL_MAX_MSGS)
    {
        return eSTATUS_INVALID_VALUE;
    }
    
    struct i2c_rdwr_ioctl_data transfer = {
        .msgs = messages,
        .nmsgs = (unsigned int)count
    };

    if(ioctl(i2c_devices[device_index].fd, I2C_RDWR, &transfer) < 0)
    {
        return eSTATUS_DEVICE_ERROR;
    }

    return eSTATUS_SUCCESSFUL;
}

eStatus hal_i2c_init(void)
{
    for(uint32_t device_index = 0; device_index < eI2C_DEVICE_COUNT; ++device_index)
    {
        if(i2c_devices[device_index].fd == -1)
        {
            i2c_devices[device_index].fd = open(i2c_devices[device_index].path, O_RDWR);
            if(i2c_devices[device_index].fd < 0)
            {
                return eSTATUS_DEVICE_ERROR;
            }

            uint32_t funcs = 0;
            // This call to `ioctl` saves in `funcs` a bitmask indicating the device's supported operations
            if(ioctl(i2c_devices[device_index].fd, I2C_FUNCS, &funcs) < 0)
            {
                return eSTATUS_DEVICE_ERROR;
            }
            // We check to see if the device even supports read and write operations (using the I2C_RDWR operation)
            if(!(funcs & I2C_FUNC_I2C))
            {
                return eSTATUS_DEVICE_ERROR;
            }
        }
        // Set 7bit addressing
        i2c_devices[device_index].flags &= (uint16_t)~(I2C_M_TEN);
    }

    return eSTATUS_SUCCESSFUL;
}

eStatus hal_i2c_set_address(uint32_t device_index, uint8_t address)
{
    if(device_index >= eI2C_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }

    i2c_devices[device_index].address = address;

    return eSTATUS_SUCCESSFUL;
}

eStatus hal_i2c_write(uint32_t device_index, void* buffer, size_t num_bytes)
{
    if(device_index >= eI2C_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }
    if(buffer == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    struct i2c_msg messages[I2C_SINGLE_MESSAGE] = {
        {
            .addr = i2c_devices[device_index].address,
            .flags = i2c_devices[device_index].flags,
            .len = (unsigned short)num_bytes,
            .buf = buffer
        }
    };

    return hal_i2c_transfer(device_index, messages, I2C_SINGLE_MESSAGE);
}

eStatus hal_i2c_write_reg(uint32_t device_index, uint16_t reg, size_t reg_len, void* buffer, size_t num_bytes)
{
    if(device_index >= eI2C_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }
    if(buffer == NULL || num_bytes + reg_len > I2C_MAX_MESSAGE_SIZE_BYTES)
    {
        return eSTATUS_NULL_PARAM;
    }

    uint8_t reg_buffer_combined[I2C_MAX_MESSAGE_SIZE_BYTES];
    for(size_t i = 0; i < reg_len; ++i)
    {
        reg_buffer_combined[i] = (uint8_t)(reg >> (sizeof(uint8_t) * reg_len - 1 - i));
    }
    memcpy(&reg_buffer_combined[reg_len], buffer, num_bytes);

    struct i2c_msg message = {
        .addr = i2c_devices[device_index].address,
        .flags = i2c_devices[device_index].flags,
        .len = (unsigned short)(reg_len + num_bytes),
        .buf = reg_buffer_combined
    };
    eStatus ret_val = hal_i2c_transfer(device_index, &message, I2C_SINGLE_MESSAGE);
    return ret_val;
}

eStatus hal_i2c_read(uint32_t device_index, void* buffer, size_t num_bytes)
{
    if(device_index >= eI2C_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }
    if(buffer == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    struct i2c_msg message = {
        .addr = i2c_devices[device_index].address,
        .flags = i2c_devices[device_index].flags | I2C_M_RD,    // We add the READ bit to signify a read message
        .len = (unsigned short)num_bytes,
        .buf = buffer
    };

    return hal_i2c_transfer(device_index, &message, I2C_SINGLE_MESSAGE);
}

eStatus hal_i2c_read_reg(uint32_t device_index, uint16_t reg, size_t reg_len, void* buffer, size_t num_bytes)
{
    if(device_index >= eI2C_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }
    if(buffer == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    struct i2c_msg messages[I2C_DOUBLE_MESSAGE] = {
        {
            .addr = i2c_devices[device_index].address,
            .flags = i2c_devices[device_index].flags,
            .len = (unsigned short)reg_len,
            .buf = (uint8_t*)&reg
        },
        {
            .addr = i2c_devices[device_index].address,
            .flags = i2c_devices[device_index].flags | I2C_M_RD,
            .len = (unsigned short)num_bytes,
            .buf = buffer
        }
    };

    return hal_i2c_transfer(device_index, messages, I2C_DOUBLE_MESSAGE);
}

void hal_i2c_cleanup(void)
{
    for(uint32_t device_index = 0; device_index < eI2C_DEVICE_COUNT; ++device_index)
    {
        (void)close(i2c_devices[device_index].fd);
        i2c_devices[device_index].fd = -1;
    }
}