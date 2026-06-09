#include "wind_fsm.h"

/* Standard library includes */
#include <stdbool.h>
#include <stdint.h>

/* User library includes */
#include "ddl/wind/wind_config.h"
#include "ddl/wind/wind_types.h"
#include "hal/uart/hal_uart.h"
#include "util/log/log.h"
#include "osal/osal.h"

#define FRAME_FUNCTION_CODE         0x03
#define FRAME_START_REGISTER        0x0000
#define FRAME_SPEED_REG_COUNT       0x0001
#define FRAME_SPEED_BYTE_COUNT      0x02
#define FRAME_DIRECTION_REG_COUNT   0x0001
#define FRAME_DIRECTION_BYTE_COUNT  0x02

typedef struct __attribute__((packed))
{
    uint8_t  address;
    uint8_t  function_code;
    uint16_t start_register;
    uint16_t register_count;
    uint16_t crc;
} ModbusReadCmd;

typedef struct __attribute__((packed))
{
    uint8_t  address;
    uint8_t  function_code;
    uint8_t  byte_count;
    uint16_t wind_speed_raw;   /* big-endian on wire, 10x the true m/s value */
    uint16_t crc;              /* little-endian on wire */
} WindSpeedResp;

typedef struct __attribute__((packed))
{
    uint8_t  address;
    uint8_t  function_code;
    uint8_t  byte_count;
    uint16_t direction_raw;   /* big-endian, degrees × 10 */
    uint16_t crc;
} WindDirectionResp;

static TimerArg timer_arg;

/* The FSM only reads one sensor at a time */
static union
{
    WindSpeedResp     as_speed;
    WindDirectionResp as_direction;
} resp_frame;

/* Read command buffers. Built as uint8_t arrays for mixed-endieness
 * compatability */
static uint8_t read_cmd_speed[sizeof(ModbusReadCmd)];
static uint8_t read_cmd_direction[sizeof(ModbusReadCmd)];

/* Standard Modbus-RTU CRC-16 */
static uint16_t modbus_crc16(const uint8_t* buffer, uint32_t length)
{
    uint16_t crc = 0xFFFFu;

    for(uint32_t i = 0; i < length; i++)
    {
        crc ^= (uint16_t)buffer[i];
        for(uint32_t j = 0; j < 8; j++)
        {
            if(crc & 0x0001u)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}

/* Read a big-endian uint16 from a field as it was received on the wire */
static uint16_t from_be16(uint16_t value)
{
    uint8_t* buf = (uint8_t*)&value;

    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

/* Read a little-endian uint16 from a field as it was received on the wire */
static uint16_t from_le16(uint16_t value)
{
    uint8_t* buf = (uint8_t*)&value;

    return (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
}

static void build_read_cmd(uint8_t* buf, uint8_t address, uint16_t reg_count)
{
    buf[0] = address;
    buf[1] = (uint8_t)FRAME_FUNCTION_CODE;
    buf[2] = (uint8_t)((FRAME_START_REGISTER >> 8) & 0xFFu);
    buf[3] = (uint8_t)(FRAME_START_REGISTER & 0xFFu);
    buf[4] = (uint8_t)((reg_count >> 8) & 0xFFu);
    buf[5] = (uint8_t)(reg_count & 0xFFu);

    uint16_t crc = modbus_crc16(buf, 6);
    buf[6] = (uint8_t)(crc & 0xFFu);          /* CRC low byte first on wire */
    buf[7] = (uint8_t)((crc >> 8) & 0xFFu);   /* CRC high byte */
}

static bool is_speed_frame_valid(const WindSpeedResp* frame)
{
    if(frame->address != (uint8_t)eWIND_SPEED_ADDRESS)
    {
        return false;
    }

    if(frame->function_code != (uint8_t)FRAME_FUNCTION_CODE)
    {
        return false;
    }

    if(frame->byte_count != (uint8_t)FRAME_SPEED_BYTE_COUNT)
    {
        return false;
    }

    uint16_t expected_crc = modbus_crc16((const uint8_t*)frame,
                            sizeof(WindSpeedResp) - sizeof(frame->crc));

    if(from_le16(frame->crc) != expected_crc)
    {
        return false;
    }

    return true;
}

static bool is_direction_frame_valid(const WindDirectionResp* frame)
{
    if(frame->address != (uint8_t)eWIND_DIRECTION_ADDRESS)
    {
        return false;
    }

    if(frame->function_code != (uint8_t)FRAME_FUNCTION_CODE)
    {
        return false;
    }

    if(frame->byte_count != (uint8_t)FRAME_DIRECTION_BYTE_COUNT)
    {
        return false;
    }

    uint16_t expected_crc = modbus_crc16((const uint8_t*)frame,
                            sizeof(WindDirectionResp) - sizeof(frame->crc));

    if(from_le16(frame->crc) != expected_crc)
    {
        return false;
    }

    return true;
}

static void update_speed_frame(WindObject* aobj, const WindSpeedResp* frame)
{
    aobj->frame->speed_valid = true;
    uint16_t raw_speed = from_be16(frame->wind_speed_raw);
    /* The sensor reports 10x the true wind speed value */
    aobj->frame->speed = (float)raw_speed / 10.0f;
}

static void update_direction_frame(WindObject* aobj, const WindDirectionResp* frame)
{
    uint16_t raw = from_be16(frame->direction_raw);
    aobj->frame->direction_valid   = true;
    aobj->frame->direction_degrees = (float)raw / 10.0f;
}

static void retry_handler(WindObject* aobj, FSM* fsm)
{
    aobj->retry++;
    if(aobj->retry < eWIND_READ_RETRY_MAX)
    {
        (void)util_fsm_transition(fsm, wind_read_state);
        return;
    }

    LOG_DEBUG("Retries exceeded for %s sensor",
              (aobj->current_target == eWIND_TARGET_SPEED) ? "speed" : "direction");

    if(aobj->current_target == eWIND_TARGET_SPEED)
    {
        /* Speed failed — invalidate it, but still try direction. */
        aobj->frame->speed_valid = false;
        aobj->current_target = eWIND_TARGET_DIRECTION;
        aobj->retry = 0;
        (void)util_fsm_transition(fsm, wind_read_state);
    }
    else
    {
        /* Direction failed — invalidate it and end the cycle. */
        aobj->frame->direction_valid = false;
        (void)util_fsm_transition(fsm, wind_idle_state);
    }
}

static void timeout_handler(void* arg)
{
    static Event timeout_event = { .type = eWIND_EVENT_TIMEOUT };
    WindObject* aobj = (WindObject*)arg;
    (void)util_active_object_post(&aobj->aobj, &timeout_event);
}

static void uart_rx_complete_handler(void* arg)
{
    static Event frame_received_event = { .type = eWIND_EVENT_FRAME_RECEIVED };
    WindObject* aobj = (WindObject*)arg;
    (void)util_active_object_post(&aobj->aobj, &frame_received_event);
}

void wind_init_state(FSM* fsm, Event* event)
{
    WindObject* aobj = (WindObject*)fsm->arg;
    switch(event->type)
    {
    case eFSM_EVENT_INIT:
        LOG_DEBUG("INIT entry");
        aobj->frame->speed_valid = false;
        aobj->frame->direction_valid = false;

        build_read_cmd(read_cmd_speed, (uint8_t)eWIND_SPEED_ADDRESS,
                       (uint16_t)FRAME_SPEED_REG_COUNT);
        build_read_cmd(read_cmd_direction, (uint8_t)eWIND_DIRECTION_ADDRESS,
                       (uint16_t)FRAME_DIRECTION_REG_COUNT);

        timer_arg.handler = timeout_handler;
        timer_arg.arg = aobj;
        if(osal_timer_init(&aobj->timer, &timer_arg))
        {
            (void)util_fsm_transition(fsm, wind_error_state);
        }
        else
        {
            (void)util_fsm_transition(fsm, wind_idle_state);
        }
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("INIT exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void wind_error_state(FSM* fsm, Event* event)
{
    WindObject* aobj = (WindObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_ERROR("ERROR entry");
        aobj->frame->speed_valid     = false;
        aobj->frame->direction_valid = false;
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void wind_idle_state(FSM* fsm, Event* event)
{
    WindObject* aobj = (WindObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("IDLE entry");
        aobj->retry = 0;
        aobj->current_target = eWIND_TARGET_SPEED;
        break;
    case eWIND_EVENT_READ:
        LOG_DEBUG("Read event received");
        (void)util_fsm_transition(fsm, wind_read_state);
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("IDLE exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void wind_read_state(FSM* fsm, Event* event)
{
    WindObject* aobj = (WindObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        if(aobj->current_target == eWIND_TARGET_SPEED)
        {
            LOG_DEBUG("READ entry (speed)");
            (void)hal_uart_write(eWIND_UART_DEVICE, read_cmd_speed, sizeof(read_cmd_speed), NULL, NULL);
            (void)hal_uart_read(eWIND_UART_DEVICE, &resp_frame.as_speed, sizeof(resp_frame.as_speed), uart_rx_complete_handler, aobj);
        }
        else
        {
            LOG_DEBUG("READ entry (direction)");
            (void)hal_uart_write(eWIND_UART_DEVICE, read_cmd_direction, sizeof(read_cmd_direction), NULL, NULL);
            (void)hal_uart_read(eWIND_UART_DEVICE, &resp_frame.as_direction, sizeof(resp_frame.as_direction), uart_rx_complete_handler, aobj);
        }
        (void)osal_timer_arm(aobj->timer, eWIND_READ_TIMEOUT_MS, eTIMER_TYPE_ONCE);
        break;
    case eWIND_EVENT_FRAME_RECEIVED:
        LOG_DEBUG("Frame Received!");
        (void)util_fsm_transition(fsm, wind_update_state);
        break;
    case eWIND_EVENT_TIMEOUT:
        LOG_DEBUG("Read timed out");
        (void)hal_uart_abort(eWIND_UART_DEVICE);
        retry_handler(aobj, fsm);
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("READ exit");
        (void)osal_timer_disarm(aobj->timer);
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void wind_update_state(FSM* fsm, Event* event)
{
    WindObject* aobj = (WindObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("UPDATE entry");
        if(aobj->current_target == eWIND_TARGET_SPEED)
        {
            if(is_speed_frame_valid(&resp_frame.as_speed))
            {
                update_speed_frame(aobj, &resp_frame.as_speed);
                LOG_DEBUG("Speed valid: %.1f m/s", (double)aobj->frame->speed);
                /* Advance to the direction sensor with a fresh retry budget. */
                aobj->current_target = eWIND_TARGET_DIRECTION;
                aobj->retry = 0;
                (void)util_fsm_transition(fsm, wind_read_state);
            }
            else
            {
                LOG_WARNING("Speed frame invalid");
                retry_handler(aobj, fsm);
            }
        }
        else
        {
            if(is_direction_frame_valid(&resp_frame.as_direction))
            {
                update_direction_frame(aobj, &resp_frame.as_direction);
                LOG_DEBUG("Direction valid: %.1f degrees",
                          (double)aobj->frame->direction_degrees);
                /* Cycle complete — back to idle. */
                (void)util_fsm_transition(fsm, wind_idle_state);
            }
            else
            {
                LOG_WARNING("Direction frame invalid");
                retry_handler(aobj, fsm);
            }
        }
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("UPDATE exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}