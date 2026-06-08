#include "compass_fsm.h"

/* Standard library includes */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

/* User library includes */
#include "ddl/compass/compass_config.h"
#include "ddl/compass/compass_types.h"
#include "hal/i2c/hal_i2c.h"
#include "util/log/log.h"
#include "osal/osal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static eStatus compass_write_reg(uint8_t reg, uint8_t value)
{
    return hal_i2c_write_reg(eCOMPASS_I2C_DEVICE, (uint16_t)reg, 1, &value, 1);
}

static eStatus compass_read_reg(uint8_t reg, uint8_t* value)
{
    return hal_i2c_read_reg(eCOMPASS_I2C_DEVICE, (uint16_t)reg, 1, value, 1);
}

static eStatus compass_read(uint8_t reg, uint8_t* buffer, size_t len)
{
    return hal_i2c_read_reg(eCOMPASS_I2C_DEVICE, (uint16_t)reg, 1, buffer, len);
}

static eStatus compass_acquire_once(uint32_t* out_x, uint32_t* out_y, uint32_t* out_z)
{
    /* Trigger a single magnetic measurement */
    eStatus status = compass_write_reg(COMPASS_REG_INTERNAL_CTRL_0, COMPASS_CTRL0_TM_M);
    if(status != eSTATUS_SUCCESSFUL)
    {
        LOG_WARNING("Failed to issue TM_M (status=%d)", status);
        return status;
    }

    /* Poll Status.Meas_M_Done */
    uint8_t  status_reg = 0;
    uint32_t waited_ms  = 0;
    while(waited_ms < COMPASS_MEAS_TIMEOUT_MS)
    {
        osal_delay_ms(COMPASS_MEAS_POLL_INTERVAL_MS);
        waited_ms += COMPASS_MEAS_POLL_INTERVAL_MS;

        status = compass_read_reg(COMPASS_REG_STATUS, &status_reg);
        if(status != eSTATUS_SUCCESSFUL)
        {
            LOG_WARNING("Failed to read Status register (status=%d)", status);
            return status;
        }

        if(status_reg & COMPASS_STATUS_MEAS_M_DONE)
        {
            break;
        }
    }

    if(!(status_reg & COMPASS_STATUS_MEAS_M_DONE))
    {
        LOG_WARNING("Measurement did not complete within %u ms", (unsigned)COMPASS_MEAS_TIMEOUT_MS);
        return eSTATUS_DEVICE_ERROR;
    }

    uint8_t raw[7];
    status = compass_read(COMPASS_REG_XOUT_0, raw, sizeof(raw));
    if(status != eSTATUS_SUCCESSFUL)
    {
        LOG_WARNING("Failed to read XYZ output bytes (status=%d)", status);
        return status;
    }

    *out_x = ((uint32_t)raw[0] << 10) |
             ((uint32_t)raw[1] <<  2) |
             (((uint32_t)raw[6] >> 6) & 0x03U);

    *out_y = ((uint32_t)raw[2] << 10) |
             ((uint32_t)raw[3] <<  2) |
             (((uint32_t)raw[6] >> 4) & 0x03U);

    *out_z = ((uint32_t)raw[4] << 10) |
             ((uint32_t)raw[5] <<  2) |
             (((uint32_t)raw[6] >> 2) & 0x03U);

    return eSTATUS_SUCCESSFUL;
}

static eStatus compass_read_temperature(float* out_temp_c)
{
    eStatus status = compass_write_reg(COMPASS_REG_INTERNAL_CTRL_0, COMPASS_CTRL0_TM_T);
    if(status != eSTATUS_SUCCESSFUL)
    {
        return status;
    }

    /* Poll Status.Meas_T_Done */
    uint8_t  status_reg = 0;
    uint32_t waited_ms  = 0;
    while(waited_ms < COMPASS_MEAS_TIMEOUT_MS)
    {
        osal_delay_ms(COMPASS_MEAS_POLL_INTERVAL_MS);
        waited_ms += COMPASS_MEAS_POLL_INTERVAL_MS;

        status = compass_read_reg(COMPASS_REG_STATUS, &status_reg);
        if(status != eSTATUS_SUCCESSFUL)
        {
            return status;
        }

        if(status_reg & COMPASS_STATUS_MEAS_T_DONE)
        {
            break;
        }
    }

    if(!(status_reg & COMPASS_STATUS_MEAS_T_DONE))
    {
        return eSTATUS_DEVICE_ERROR;
    }

    uint8_t tout = 0;
    status = compass_read_reg(COMPASS_REG_TOUT, &tout);
    if(status != eSTATUS_SUCCESSFUL)
    {
        return status;
    }

    *out_temp_c = ((float)tout * COMPASS_TEMP_SCALE_C_PER_LSB) + COMPASS_TEMP_OFFSET_C;
    return eSTATUS_SUCCESSFUL;
}

/* Implements the procedure from datasheet ("USING SET AND
 * RESET TO REMOVE BRIDGE OFFSET"):
 *
 * 1) SET                       (positive coil polarisation)
 * 2) Measure -> Output1 =  +H + Offset
 * 3) RESET                     (negative coil polarisation)
 * 4) Measure -> Output2 =  -H + Offset
 * 5) H = (Output1 - Output2) / 2     (Offset cancels out)
 *
 * `heading_deg` is left as NAN; compass_compute_heading() fills it.
 */
static void compass_read_raw(MagFrame* frame)
{
    frame->valid = false;
    frame->heading_deg = NAN;

    uint32_t out1_x = 0, out1_y = 0, out1_z = 0;
    uint32_t out2_x = 0, out2_y = 0, out2_z = 0;

    /* --- Half cycle 1: SET, then measure. ----------------------------- */
    if(compass_write_reg(COMPASS_REG_INTERNAL_CTRL_0, COMPASS_CTRL0_SET) != eSTATUS_SUCCESSFUL)
    {
        LOG_WARNING("SET pulse write failed");
        return;
    }
    osal_delay_ms(COMPASS_SET_RESET_SETTLE_MS);

    if(compass_acquire_once(&out1_x, &out1_y, &out1_z) != eSTATUS_SUCCESSFUL)
    {
        LOG_WARNING("First acquisition (post-SET) failed");
        return;
    }

    /* --- Half cycle 2: RESET, then measure. --------------------------- */
    if(compass_write_reg(COMPASS_REG_INTERNAL_CTRL_0, COMPASS_CTRL0_RESET) != eSTATUS_SUCCESSFUL)
    {
        LOG_WARNING("RESET pulse write failed");
        return;
    }
    osal_delay_ms(COMPASS_SET_RESET_SETTLE_MS);

    if(compass_acquire_once(&out2_x, &out2_y, &out2_z) != eSTATUS_SUCCESSFUL)
    {
        LOG_WARNING("Second acquisition (post-RESET) failed");
        return;
    }

    /* --- Offset-removal subtraction. ---------------------------------- *
     * Cast to int32_t before subtraction because each 18-bit value can
     * legitimately occupy the high end of the uint32_t range when the
     * field is strong on a given axis. */
    int32_t signed_x = ((int32_t)out1_x - (int32_t)out2_x) / 2;
    int32_t signed_y = ((int32_t)out1_y - (int32_t)out2_y) / 2;
    int32_t signed_z = ((int32_t)out1_z - (int32_t)out2_z) / 2;

    frame->raw_x = (uint32_t)(signed_x + (int32_t)COMPASS_18BIT_NULL_FIELD);
    frame->raw_y = (uint32_t)(signed_y + (int32_t)COMPASS_18BIT_NULL_FIELD);
    frame->raw_z = (uint32_t)(signed_z + (int32_t)COMPASS_18BIT_NULL_FIELD);

    /* --- Temperature is non-critical; failure leaves the previous
     * temperature in place but does not invalidate the magnetic read. - */
    float temp_c = 0.0f;
    if(compass_read_temperature(&temp_c) == eSTATUS_SUCCESSFUL)
    {
        frame->temperature_c = temp_c;
    }
    else
    {
        LOG_WARNING("Temperature read failed; keeping previous value");
    }

    frame->valid = true;
}

static void compass_compute_heading(MagFrame* frame, float pitch_deg, float roll_deg)
{
    /* Convert offset-binary 18-bit values into signed Gauss */
    float mx = ((float)frame->raw_x - (float)COMPASS_18BIT_NULL_FIELD) /
                COMPASS_18BIT_COUNTS_PER_GAUSS;
    float my = ((float)frame->raw_y - (float)COMPASS_18BIT_NULL_FIELD) /
                COMPASS_18BIT_COUNTS_PER_GAUSS;
    float mz = ((float)frame->raw_z - (float)COMPASS_18BIT_NULL_FIELD) /
                COMPASS_18BIT_COUNTS_PER_GAUSS;

    /* Tilt compensation */
    float pitch_rad = pitch_deg * (float)(M_PI / 180.0);
    float roll_rad  = roll_deg  * (float)(M_PI / 180.0);

    float cos_p = cosf(pitch_rad);
    float sin_p = sinf(pitch_rad);
    float cos_r = cosf(roll_rad);
    float sin_r = sinf(roll_rad);

    float mx_h = mx * cos_p + mz * sin_p;
    float my_h = mx * sin_r * sin_p
               + my * cos_r
               - mz * sin_r * cos_p;

    /* Heading: clockwise from magnetic-north, in degrees */
    float heading = atan2f(-my_h, mx_h) * (180.0f / (float)M_PI);

#if COMPASS_INVERT_HEADING
    heading = -heading;
#endif

    heading += COMPASS_DECLINATION_DEG;
    heading += COMPASS_HEADING_OFFSET_DEG;

    /* Normalise into [0, 360) */
    heading = fmodf(heading, 360.0f);
    if(heading < 0.0f)
    {
        heading += 360.0f;
    }

    frame->heading_deg = heading;
}

static eStatus compass_init(void)
{
    eStatus status = hal_i2c_set_address(eCOMPASS_I2C_DEVICE, eCOMPASS_I2C_ADDRESS);
    if (status != eSTATUS_SUCCESSFUL)
    {
        LOG_ERROR("set_address failed: %d", status);
        return status;
    }

    uint8_t product_id = 0;
    status = compass_read_reg(COMPASS_REG_PRODUCT_ID, &product_id);
    if (status != eSTATUS_SUCCESSFUL)
    {
        LOG_ERROR("read product ID failed: %d", status);
        return status;
    }
    if (product_id != COMPASS_PRODUCT_ID_VALUE)
    {
        LOG_ERROR("Unexpected Product ID 0x%02X (want 0x%02X)", product_id, COMPASS_PRODUCT_ID_VALUE);
        return eSTATUS_DEVICE_ERROR;
    }

    status = compass_write_reg(COMPASS_REG_INTERNAL_CTRL_1, COMPASS_CTRL1_BW_8MS);
    if (status != eSTATUS_SUCCESSFUL)
    {
        LOG_ERROR("write Internal Control 1 failed: %d", status);
        return status;
    }

    return eSTATUS_SUCCESSFUL;
}

static void retry_handler(CompassObject* aobj, FSM* fsm)
{
    aobj->retry++;
    if(aobj->retry < eCOMPASS_RETRY_MAX)
    {
        (void)util_fsm_transition(fsm, compass_read_state);
    }
    else
    {
        LOG_DEBUG("Retries exceeded limit (%u)", aobj->retry);
        aobj->frame->valid = false;
        (void)util_fsm_transition(fsm, compass_idle_state);
    }
}

void compass_init_state(FSM* fsm, Event* event)
{
    CompassObject* aobj = (CompassObject*)fsm->arg;
    switch(event->type)
    {
    case eFSM_EVENT_INIT:
    {
        LOG_DEBUG("INIT entry");
        aobj->frame->valid = false;
        aobj->frame->heading_deg = NAN;
        if (compass_init())
        {
            (void)util_fsm_transition(fsm, compass_error_state);
        }
        else
        {
            (void)util_fsm_transition(fsm, compass_idle_state);
        }
        break;
    }
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("INIT exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void compass_error_state(FSM* fsm, Event* event)
{
    CompassObject* aobj = (CompassObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_ERROR("ERROR entry");
        aobj->frame->valid = false;
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void compass_idle_state(FSM* fsm, Event* event)
{
    CompassObject* aobj = (CompassObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("IDLE entry");
        aobj->retry = 0;
        break;
    case eCOMPASS_EVENT_READ:
        LOG_DEBUG("Read event received");
        (void)util_fsm_transition(fsm, compass_read_state);
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("IDLE exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void compass_read_state(FSM* fsm, Event* event)
{
    CompassObject* aobj = (CompassObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("READ entry");

        /* The whole SET/measure/RESET/measure/compute pass runs synchronously here */
        compass_read_raw(aobj->frame);

        if(!aobj->frame->valid)
        {
            LOG_WARNING("Raw read failed; retrying");
            retry_handler(aobj, fsm);
            break;
        }

        compass_compute_heading(aobj->frame,
                               COMPASS_DEFAULT_PITCH_DEG,
                               COMPASS_DEFAULT_ROLL_DEG);

        LOG_DEBUG("Frame is valid. Heading: %.1f deg, temp: %.1f C",
                  (double)aobj->frame->heading_deg,
                  (double)aobj->frame->temperature_c);

        (void)util_fsm_transition(fsm, compass_idle_state);
        break;

    case eFSM_EVENT_EXIT:
        LOG_DEBUG("READ exit");
        break;

    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}
