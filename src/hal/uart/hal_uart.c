#include "hal_uart.h"

/* Standard Libraries */
#include <linux/serial.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <liburing.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

/* Linux Specific Libraries */
#include <termios.h>

/* User Libraries */
#include "hal_uart_config.h"

#define MAX_QUEUED_OPERATIONS (eUART_MAX_QUEUED_OPERATIONS * eUART_DEVICE_COUNT)
#define MAX_QUEUE_ENTRIES     (MAX_QUEUED_OPERATIONS * 2)

//typedef struct OpSlot OpSlot;

typedef struct
{
    async_cb callback;
    void*    arg;
    void*    buffer;
    uint32_t total_len;
    uint32_t bytes_done;
    int      fd;
    bool     used;
    bool     is_read;
    uint8_t  padding[2];
}OpSlot;

typedef struct
{
    char*       path;                               /** Path to the UART device, e.g. "/dev/ttyAMA0" */
    int         fd;                                 /** File descriptor for the UART device */
    speed_t     baud;                               /** Baud rate or BPS for the device communication */
    tcflag_t    word_size;                          /** Word size (5-8 bits) */
    uint16_t    stop_bits;                          /** Single or Double stop bits */
    uint16_t    parity;                             /** Parity bits in use (None, Even, or Odd) */
    OpSlot      slots[eUART_MAX_QUEUED_OPERATIONS]; /** Operations array */
    bool        rs485_enabled;                      /** Flag for RTS config */
    uint8_t     padding[7];
} UARTDevice;

static UARTDevice uart_devices[eUART_DEVICE_COUNT] = {
    [eUART0_DEVICE] = {
        .path = "/dev/ttyAMA0",
        .fd = -1,
        .baud = eUART0_BAUD_CONFIG,
        .word_size = eUART0_BITS_PER_BYTE_CONFIG,
        .stop_bits = eUART0_STOP_BIT_CONFIG,
        .parity = eUART0_PARITY_BIT_CONFIG,
        .slots = { { 0 } },
        .rs485_enabled = eUART0_RS485_CONFIG
    },
    [eUART1_DEVICE] = {
        .path = "/dev/ttyAMA1",
        .fd = -1,
        .baud = eUART1_BAUD_CONFIG,
        .word_size = eUART1_BITS_PER_BYTE_CONFIG,
        .stop_bits = eUART1_STOP_BIT_CONFIG,
        .parity = eUART1_PARITY_BIT_CONFIG,
        .slots = { { 0 } },
        .rs485_enabled = eUART1_RS485_CONFIG
    },
    [eUART2_DEVICE] = {
        .path = "/dev/ttyAMA2",
        .fd = -1,
        .baud = eUART2_BAUD_CONFIG,
        .word_size = eUART2_BITS_PER_BYTE_CONFIG,
        .stop_bits = eUART2_STOP_BIT_CONFIG,
        .parity = eUART2_PARITY_BIT_CONFIG,
        .slots = { { 0 } },
        .rs485_enabled = eUART2_RS485_CONFIG
    }
};

static pthread_mutex_t uart_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       uart_thread  = 0;
static struct io_uring uart_ring    = { 0 };
static bool            uart_running = false;

/* Allocates a slot to submit an operation to the queue used by io-uring*/
static OpSlot* allocate_slot(UARTDevice* device)
{
    for(int i = 0; i < eUART_MAX_QUEUED_OPERATIONS; i++) 
    {
        if (device->slots[i].used == false)
        {
            device->slots[i].used = true;
            return &device->slots[i];
        }  
    }

    return NULL;
}

static void free_slot(OpSlot* slot)
{
    slot->used = false;
}

static void* io_completion_thread(void* arg)
{
    (void)arg;

    while(uart_running) 
    {
        struct io_uring_cqe* cqe = NULL;
        if(io_uring_wait_cqe(&uart_ring, &cqe) < 0)
        {
            continue;
        }

        (void)pthread_mutex_lock(&uart_mutex);

        OpSlot* slot = io_uring_cqe_get_data(cqe);
        if(slot != NULL)
        {
            /* Accumulate this completion's bytes. cqe->res can be
             * negative on error or zero on EOF, so guard the add. */
            if(cqe->res > 0)
            {
                slot->bytes_done += (uint32_t)cqe->res;
            }

            if(slot->is_read && cqe->res > 0 && slot->bytes_done < slot->total_len)
            {
                /* Short read. The tty is configured with VMIN=1 so a
                 * single io_uring_read can return as soon as >=1 byte
                 * is available, even when more was asked for. Submit
                 * another read for the remainder against the same
                 * slot - the user callback fires only when the buffer
                 * is fully populated. */
                struct io_uring_sqe* sqe = io_uring_get_sqe(&uart_ring);
                if(sqe != NULL)
                {
                    io_uring_sqe_set_data(sqe, slot);
                    io_uring_prep_read(sqe, slot->fd,
                                       (uint8_t*)slot->buffer + slot->bytes_done,
                                       slot->total_len - slot->bytes_done,
                                       (uint64_t)-1);
                    (void)io_uring_submit(&uart_ring);
                    /* Slot stays in use; do not free or fire callback. */
                }
                else
                {
                    /* Submission queue full - can't chain. Drop the
                     * operation; the FSM's read timer will fire. */
                    free_slot(slot);
                }
            }
            else
            {
                /* Write completion, full-length read, or error
                 * (cqe->res <= 0). Fire the user callback if bytes
                 * were actually transferred, then release the slot. */
                if(slot->callback != NULL && cqe->res > 0)
                {
                    slot->callback(slot->arg);
                }

                free_slot(slot);
            }
        }

        io_uring_cqe_seen(&uart_ring, cqe);

        (void)pthread_mutex_unlock(&uart_mutex);
    }

    return NULL;
}

eStatus hal_uart_init(void)
{
    const speed_t baud_options[] = {
        B0, B50, B75, B110, B134,
        B150, B200, B300, B600, B1200,
        B1800, B2400, B4800, B9600, B19200,
        B38400, B57600, B115200, B230400, B460800
    };

    const tcflag_t word_size_options[] = { CS5, CS6, CS7, CS8 };

    // Here we set the baud, flags (we don't need any), word size, stop bits, and parity bits for each UART device
    // All the bits configuration is done via the termios struct with its flags and the 'tcsetattr' function
    // The baud is set using 'cfsetospeed' and 'cfsetispeed' functions and the termios struct

    for(uint32_t device_index = 0; device_index < eUART_DEVICE_COUNT; ++device_index)
    {
        struct termios temp_config = { 0 };

        // Prevents waiting for modem connection and enables reading input from the terminal
        temp_config.c_cflag = CLOCAL | CREAD;

        // Sets the bits per byte
        temp_config.c_cflag |= word_size_options[uart_devices[device_index].word_size];

        // Determine stop bit count
        if(uart_devices[device_index].stop_bits == eSINGLE_STOP_BIT)
        {
            // Ensures CSTOPB bit is 0  (sets use of one stop bit)
            temp_config.c_cflag &= (tcflag_t)~(CSTOPB);
        }
        else
        {
            // Sets CSTOPB bit to 1 (sets use of two stop bits)
            temp_config.c_cflag |= CSTOPB;
        }

        // Determine parity bit 
        if(uart_devices[device_index].parity == eNO_PARITY_BIT)
        {
            temp_config.c_cflag &= (tcflag_t)~(PARENB);     // Ensures PAREND bit is 0 (disables parity generation and detection)
            temp_config.c_iflag &= (tcflag_t)~(INPCK);      // Ensures INPCK bit is 0 (disables input parity checking)
        }
        else
        {
            temp_config.c_cflag |= PARENB;                  // Sets PAREND bit to 1 (enables parity generation and detection)
            if(uart_devices[device_index].parity == eEVEN_PARITY_BIT)
            {
                temp_config.c_cflag &= (tcflag_t)~(PARODD); // Ensures PARODD bit is 0 (sets parity to even)
            }
            else
            {
                temp_config.c_cflag |= PARODD;              // Sets PARODD bit to 1 (sets parity to odd)
            }
            temp_config.c_iflag |= INPCK;                   // Sets INPCK bit to 1 (enables input parity checking)
            temp_config.c_iflag |= IGNPAR;                  // Sets IGNPAR bit to 1 (sets framing or parity errors to be ignored)
        }

        // Wait in blocking for at least one character with 100ms timeout
        temp_config.c_cc[VMIN]  = 1; // Minimum number of charaters to wait for
        temp_config.c_cc[VTIME] = 1; // 1 = 0.1S = 100ms timeout

        // These function return 0 if all went normal and -1 if an error occured
        if(cfsetispeed(&temp_config, baud_options[uart_devices[device_index].baud]) < 0 || 
            cfsetospeed(&temp_config, baud_options[uart_devices[device_index].baud]) < 0)       
        {
            return eSTATUS_DEVICE_ERROR;
        }

        uart_devices[device_index].fd = open(uart_devices[device_index].path, O_RDWR | O_NOCTTY);
        if(uart_devices[device_index].fd < 0)
        {
            return eSTATUS_DEVICE_ERROR;
        }

        if(!isatty(uart_devices[device_index].fd))
        {
            close(uart_devices[device_index].fd);
            return eSTATUS_DEVICE_ERROR;
        }

        if(tcsetattr(uart_devices[device_index].fd, TCSAFLUSH, &temp_config))
        {
            return eSTATUS_DEVICE_ERROR;
        }

        if(uart_devices[device_index].rs485_enabled)
        {
            struct serial_rs485 rs485_config = { 0 };
            rs485_config.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
            if(ioctl(uart_devices[device_index].fd, TIOCSRS485, &rs485_config) < 0)
            {
                return eSTATUS_DEVICE_ERROR;
            }
        }
    }

    if(io_uring_queue_init(MAX_QUEUE_ENTRIES, &uart_ring, 0) < 0)
    {
       return eSTATUS_SYSTEM_ERROR;
    }

    uart_running = true;
    if(pthread_create(&uart_thread, NULL, io_completion_thread, NULL))
    {
        io_uring_queue_exit(&uart_ring);
        uart_running = false;
        return eSTATUS_SYSTEM_ERROR;
    }

    return eSTATUS_SUCCESSFUL;
}

eStatus hal_uart_write(uint32_t device_index, const void* buffer, uint32_t len, async_cb callback, void* arg)
{
    if(device_index >= eUART_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }

    if(buffer == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    (void)pthread_mutex_lock(&uart_mutex);

    OpSlot* slot = allocate_slot(&uart_devices[device_index]);
    if(slot == NULL)
    {
        (void)pthread_mutex_unlock(&uart_mutex);
        return eSTATUS_ACTION_FAILED;
    }

    slot->callback = callback;
    slot->arg = arg;
    slot->buffer = NULL;
    slot->total_len = 0;
    slot->bytes_done = 0;
    slot->fd = -1;
    slot->is_read = false;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&uart_ring);
    if (sqe == NULL) 
    {
        free_slot(slot);
        (void)pthread_mutex_unlock(&uart_mutex);
        return eSTATUS_SYSTEM_ERROR;
    }

    io_uring_sqe_set_data(sqe, slot);

    io_uring_prep_write(sqe, uart_devices[device_index].fd, buffer, len, (uint64_t)-1);

    if(io_uring_submit(&uart_ring) < 0)
    {
        free_slot(slot);
        (void)pthread_mutex_unlock(&uart_mutex);
        return eSTATUS_SYSTEM_ERROR;
    }

    (void)pthread_mutex_unlock(&uart_mutex);

    return eSTATUS_SUCCESSFUL;
}

eStatus hal_uart_read(uint32_t device_index, void* buffer, uint32_t len, async_cb callback, void* arg)
{
    if(device_index >= eUART_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }

    if(buffer == NULL)
    {
        return eSTATUS_NULL_PARAM;
    }

    (void)pthread_mutex_lock(&uart_mutex);

    OpSlot* slot = allocate_slot(&uart_devices[device_index]);
    if(slot == NULL)
    {
        (void)pthread_mutex_unlock(&uart_mutex);
        return eSTATUS_ACTION_FAILED;
    }

    slot->callback = callback;
    slot->arg = arg;
    slot->buffer = buffer;
    slot->total_len = len;
    slot->bytes_done = 0;
    slot->fd = uart_devices[device_index].fd;
    slot->is_read = true;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&uart_ring);
    if (sqe == NULL) 
    {
        free_slot(slot);
        (void)pthread_mutex_unlock(&uart_mutex);
        return eSTATUS_SYSTEM_ERROR;
    }

    io_uring_sqe_set_data(sqe, slot);

    io_uring_prep_read(sqe, uart_devices[device_index].fd, buffer, len, (uint64_t)-1);

    if(io_uring_submit(&uart_ring) < 0)
    {
        free_slot(slot);
        (void)pthread_mutex_unlock(&uart_mutex);
        return eSTATUS_SYSTEM_ERROR;
    }

    (void)pthread_mutex_unlock(&uart_mutex);

    return eSTATUS_SUCCESSFUL;
}

eStatus hal_uart_flush_input(uint32_t device_index)
{
    if(device_index >= eUART_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }
    (void)tcflush(uart_devices[device_index].fd, TCIFLUSH);
    return eSTATUS_SUCCESSFUL;
}

eStatus hal_uart_abort(uint32_t device_index)
{
    if(device_index >= eUART_DEVICE_COUNT)
    {
        return eSTATUS_INVALID_VALUE;
    }

    (void)pthread_mutex_lock(&uart_mutex);

    struct io_uring_sqe* sqe = io_uring_get_sqe(&uart_ring);
    if(sqe == NULL) 
    {
        (void)pthread_mutex_unlock(&uart_mutex);
        return eSTATUS_SYSTEM_ERROR;
    }

    io_uring_prep_cancel_fd(sqe, uart_devices[device_index].fd, 0);
    if(io_uring_submit(&uart_ring) < 0)
    {
        (void)pthread_mutex_unlock(&uart_mutex);
        return eSTATUS_SYSTEM_ERROR;
    }

    (void)pthread_mutex_unlock(&uart_mutex);

    return eSTATUS_SUCCESSFUL;
}

void hal_uart_cleanup(void)
{
    (void)pthread_mutex_lock(&uart_mutex);

    uart_running = false;

    /* wake the thread */
    struct io_uring_sqe* sqe = io_uring_get_sqe(&uart_ring);
    if(sqe != NULL) 
    {
        io_uring_prep_nop(sqe);
        (void)io_uring_submit(&uart_ring);
    }

    (void)pthread_mutex_unlock(&uart_mutex);

    (void)pthread_join(uart_thread, NULL);

    io_uring_queue_exit(&uart_ring);

    for(uint32_t device_index = 0; device_index < eUART_DEVICE_COUNT; ++device_index)
    {
        (void)close(uart_devices[device_index].fd);
        uart_devices[device_index].fd = -1;
    }
}