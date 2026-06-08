#include "servo_fsm.h"

/* Standard library includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* User library includes */
#include "ddl/servo/servo_config.h"
#include "ddl/servo/servo_types.h"
#include "hal/i2c/hal_i2c.h"
#include "util/log/log.h"
#include "osal/osal.h"

typedef struct
{
    float   hor_angle;
    float   ver_angle;
} ServoAngles;

/* Acts as a registry for directed servo angles */
typedef struct
{
    ServoAngles angles;
    uint32_t    seq;    // identifier for fresh data between reads
} ServoTarget;

static TimerArg timer_arg;

static ServoAngles servo_scan_state_angles;
static bool angle_direction;

static ServoTarget servo_target_angles;
static void* servo_target_mutex;

static void sleep_us(long us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

static float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static void timeout_handler(void* arg)
{
    static Event timeout_event = { .type = eSERVO_EVENT_ROTATION_TIMEOUT };
    ServoObject* aobj = (ServoObject*)arg;
    (void)util_active_object_post(&aobj->aobj, &timeout_event);
}

static eStatus pca9685_write8(uint8_t reg, uint8_t value)
{
    return hal_i2c_write_reg(eSERVO_I2C_DEVICE, reg, 1, &value, 1);
}

static eStatus pca9685_read8(uint8_t reg, uint8_t* out_value)
{
    if (out_value == NULL) return eSTATUS_NULL_PARAM;
    return hal_i2c_read_reg(eSERVO_I2C_DEVICE, reg, 1, out_value, 1);
}

static eStatus pca9685_set_pwm_freq(float freq_hz)
{
    // prescale = round(oscillator_freq / (4096 * desired_freq)) - 1
    float prescale_float = (PCA9685_OSC_HZ / (PCA9685_PWM_STEPS * freq_hz)) - 1.0f;
    int prescale_int = (int)(prescale_float + 0.5f);
    if (prescale_int < PCA9685_PRESCALE_MIN)
        prescale_int = PCA9685_PRESCALE_MIN;
    if (prescale_int > PCA9685_PRESCALE_MAX)
        prescale_int = PCA9685_PRESCALE_MAX;
    uint8_t prescale = (uint8_t)prescale_int;

    uint8_t mode1_old;
    eStatus status = pca9685_read8(REG_MODE1, &mode1_old);
    if (status != eSTATUS_SUCCESSFUL)
        return status;

    uint8_t mode1_sleep = (uint8_t)((mode1_old & ~MODE1_RESTART) | MODE1_SLEEP);
    status = pca9685_write8(REG_MODE1, mode1_sleep);
    if (status != eSTATUS_SUCCESSFUL)
        return status;

    status = pca9685_write8(REG_PRE_SCALE, prescale);
    if (status != eSTATUS_SUCCESSFUL)
        return status;

    status = pca9685_write8(REG_MODE1, mode1_old);
    if (status != eSTATUS_SUCCESSFUL)
        return status;

    /* Oscillator can take up to 500 us to stabilise */
    sleep_us(500);

    /* Set RESTART and AI on top of the original mode bits */
    return pca9685_write8(REG_MODE1, (uint8_t)(mode1_old | MODE1_RESTART | MODE1_AI));
}

static eStatus pca9685_init(void)
{
    uint8_t swrst = 0x06;
    
    eStatus status = hal_i2c_set_address(eSERVO_I2C_DEVICE, 0x00);
    if (status != eSTATUS_SUCCESSFUL)
    {
        LOG_ERROR("SWRST set_address failed: %d", status);
        return status;
    }

    status = hal_i2c_write(eSERVO_I2C_DEVICE, &swrst, 1);
    if (status != eSTATUS_SUCCESSFUL)
    {
        LOG_ERROR("SWRST write failed: %d", status);
        return status;
    }

    sleep_us(500);

    status = hal_i2c_set_address(eSERVO_I2C_DEVICE, eSERVO_PCA_ADDRESS);
    if (status != eSTATUS_SUCCESSFUL)
    {
        LOG_ERROR("set_address failed: %d", status);
        return status;
    }
    /* MODE2: totem-pole, non-inverting, change-on-STOP */
    status = pca9685_write8(REG_MODE2, MODE2_OUTDRV);
    if (status != eSTATUS_SUCCESSFUL)
    {
        LOG_ERROR("write MODE2 failed: %d", status);
        return status;
    }

    /* MODE1: clear SLEEP so oscillator runs, keep ALLCALL for compatibility.
     * AI and RESTART get set inside pca9685_set_pwm_freq. */
    status = pca9685_write8(REG_MODE1, MODE1_ALLCALL);
    if (status != eSTATUS_SUCCESSFUL)
    {
        LOG_ERROR("write MODE1 failed: %d", status);
        return status;
    }

    sleep_us(500);

    status = pca9685_set_pwm_freq(PWM_FREQ_HZ);
    if(status != eSTATUS_SUCCESSFUL)
    {
        LOG_ERROR("set_pwm_freq failed: %d", status);
        return status;
    }

    return eSTATUS_SUCCESSFUL;
}

static eStatus pca9685_set_pwm(uint8_t channel, uint16_t on, uint16_t off)
{
    if (channel > 15)
        return eSTATUS_INVALID_VALUE;

    uint8_t data[4];
    data[0] = (uint8_t)(on & 0xFF);
    data[1] = (uint8_t)((on >> 8) & 0x0F);
    data[2] = (uint8_t)(off & 0xFF);
    data[3] = (uint8_t)((off >> 8) & 0x0F);

    return hal_i2c_write_reg(eSERVO_I2C_DEVICE, (uint16_t)LEDn_BASE(channel), 1,
                             data, sizeof(data));
}

static eStatus pca9685_get_pwm(uint8_t channel, uint16_t* out_on,
                                uint16_t* out_off)
{
    if (channel > 15)
        return eSTATUS_INVALID_VALUE;

    uint8_t data[4] = {0};
    eStatus status = hal_i2c_read_reg(eSERVO_I2C_DEVICE, (uint16_t)LEDn_BASE(channel), 1,
                                  data, sizeof(data));
    if (status != eSTATUS_SUCCESSFUL)
        return status;

    if (out_on)  *out_on  = (uint16_t)(data[0] | ((data[1] & 0x0F) << 8));
    if (out_off) *out_off = (uint16_t)(data[2] | ((data[3] & 0x0F) << 8));
    return eSTATUS_SUCCESSFUL;
}

/* Number of PWM counts per microsecond at the configured refresh rate. */
static float counts_per_us(void)
{
    float period_us = 1000000.0f / PWM_FREQ_HZ;
    return (float)PCA9685_PWM_STEPS / period_us;
}

static uint16_t pulse_us_to_counts(float pulse_us)
{
    float counts = pulse_us * counts_per_us();
    if (counts < 0.0f)
        counts = 0.0f;
    if (counts > (float)(PCA9685_PWM_STEPS-1))
        counts = PCA9685_PWM_STEPS - 1;
    return (uint16_t)(counts + 0.5f);
}

static float counts_to_pulse_us(uint16_t counts)
{
    return (float)counts / counts_per_us();
}

static eStatus servo_set_angle(uint8_t channel, float angle_deg)
{
    angle_deg = clampf(angle_deg, SERVO_MIN_ANGLE_DEG, SERVO_MAX_ANGLE_DEG);

    float pulse_us =
        SERVO_MIN_PULSE_US +
        (angle_deg / (SERVO_MAX_ANGLE_DEG - SERVO_MIN_ANGLE_DEG)) *
        (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US);

    return pca9685_set_pwm(channel, 0, pulse_us_to_counts(pulse_us));
}

static eStatus servo_get_angle(uint8_t channel, float* out_angle_deg)
{
    if (out_angle_deg == NULL) return eSTATUS_NULL_PARAM;

    uint16_t off_counts = 0;
    eStatus  status = pca9685_get_pwm(channel, NULL, &off_counts);
    if (status != eSTATUS_SUCCESSFUL)
        return status;

    float pulse_us = counts_to_pulse_us(off_counts);
    float angle =
        ((pulse_us - SERVO_MIN_PULSE_US) / (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US)) *
        (SERVO_MAX_ANGLE_DEG - SERVO_MIN_ANGLE_DEG);

    *out_angle_deg = clampf(angle, SERVO_MIN_ANGLE_DEG, SERVO_MAX_ANGLE_DEG);
    return eSTATUS_SUCCESSFUL;
}

static eStatus servo_set_both_angles(ServoObject* aobj, ServoAngles angles)
{
    eStatus status;
    status = servo_set_angle(eSERVO_HORIZONTAL_CHANNEL, angles.hor_angle);
    if(status)
    {
        return status;
    }
    status = servo_set_angle(eSERVO_VERTICAL_CHANNEL, angles.ver_angle);
    
    if(status == eSTATUS_SUCCESSFUL)
    {
        aobj->frame->hor_angle = angles.hor_angle;
        aobj->frame->ver_angle = angles.ver_angle;
    }

    return status;
}

/* Implementing Serpantine scan */
static void servo_scan_operation()
{
    if(angle_direction == SERVO_INCREASE_ANGLE && 
        servo_scan_state_angles.hor_angle < SERVO_HORIZONTAL_MAX_ANGLE_DEG)
    {
        servo_scan_state_angles.hor_angle += SERVO_STEP_ANGLE;
    }
    else if(angle_direction == SERVO_DECREASE_ANGLE &&
        servo_scan_state_angles.hor_angle > SERVO_HORIZONTAL_MIN_ANGLE_DEG)
    {
        servo_scan_state_angles.hor_angle -= SERVO_STEP_ANGLE;
    }
    else
    {
        if(servo_scan_state_angles.ver_angle < SERVO_VERTICAL_MAX_ANGLE_DEG)
        {
            servo_scan_state_angles.ver_angle += SERVO_STEP_ANGLE;
            angle_direction = !angle_direction;
        }
        else
        {
            servo_scan_state_angles.hor_angle = SERVO_HORIZONTAL_MIN_ANGLE_DEG;
            servo_scan_state_angles.ver_angle = SERVO_VERTICAL_MIN_ANGLE_DEG;
            angle_direction = SERVO_INCREASE_ANGLE;
        }
    }
}

/* An internal initialization function for the angles saved */
static eStatus servo_init_angles()
{
    eStatus status;
    servo_target_angles.angles.hor_angle = 0.0f;
    servo_target_angles.angles.ver_angle = 90.f;
    servo_target_angles.seq = 0;
    servo_scan_state_angles.hor_angle = 0.0f;
    servo_scan_state_angles.ver_angle = 90.0f;
    angle_direction = SERVO_INCREASE_ANGLE;
    status = servo_set_angle(eSERVO_HORIZONTAL_CHANNEL, servo_scan_state_angles.hor_angle);
    if(status)
    {
        return status;
    }
    status = servo_set_angle(eSERVO_VERTICAL_CHANNEL, servo_scan_state_angles.ver_angle);
    return status;
}

/* Create a copy of the target angles so the FSM can act on the target without
 * being dependant on the popular `servo_target_angle` instance */
static void servo_copy_target(ServoTarget* out)
{
    osal_mutex_lock(servo_target_mutex);
    *out = servo_target_angles;
    osal_mutex_unlock(servo_target_mutex);
}

void servo_fsm_destroy()
{
    if(servo_target_mutex != NULL)
    {
        osal_mutex_destroy(servo_target_mutex);
        servo_target_mutex = NULL;
    }
}

/* Public setter. Caller must follow this with `util_event_bus_publish()` with
 * the fitting event to actually move the FSM into the corresponding state */
eStatus servo_fsm_set_target(float hor_angle, float ver_angle)
{
    if(hor_angle < SERVO_HORIZONTAL_MIN_ANGLE_DEG || hor_angle > SERVO_HORIZONTAL_MAX_ANGLE_DEG)
    {
        return eSTATUS_INVALID_VALUE;
    }
    if(ver_angle < SERVO_VERTICAL_MIN_ANGLE_DEG || ver_angle > SERVO_VERTICAL_MAX_ANGLE_DEG)
    {
        return eSTATUS_INVALID_VALUE;
    }
    if(servo_target_mutex == NULL)
    {
        return eSTATUS_ACTION_FAILED;
    }

    osal_mutex_lock(servo_target_mutex);
    servo_target_angles.angles.hor_angle = hor_angle;
    servo_target_angles.angles.ver_angle = ver_angle;
    servo_target_angles.seq++;
    osal_mutex_unlock(servo_target_mutex);

    return eSTATUS_SUCCESSFUL;
}

void servo_init_state(FSM* fsm, Event* event)
{
    ServoObject* aobj = (ServoObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_INIT:
        LOG_DEBUG("INIT entry");
        timer_arg.handler = timeout_handler;
        timer_arg.arg = aobj;
        if(pca9685_init() || osal_mutex_init(&servo_target_mutex) ||
            osal_timer_init(&aobj->timer, &timer_arg))
        {
            (void)util_fsm_transition(fsm, servo_error_state);
        }
        else
        {
            (void)util_fsm_transition(fsm, servo_idle_state);
        }
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("INIT exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void servo_error_state(FSM* fsm, Event* event)
{
    ServoObject* aobj = (ServoObject*)fsm->arg;
    (void)aobj;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_ERROR("ERROR entry");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void servo_idle_state(FSM* fsm, Event* event)
{
    ServoObject* aobj = (ServoObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("IDLE entry");
        if(servo_init_angles())
        {
            LOG_ERROR("Failed to set servos' angles");
        }
        else
        {
            aobj->frame->hor_angle = servo_scan_state_angles.hor_angle;
            aobj->frame->ver_angle = servo_scan_state_angles.ver_angle;
        }
        break;
    // Logically it should be on SCAN event, but this is the events
    // the scheduler publishes repeatedly, and there is no reason
    // to subscribing to the scheduler with another event
    case eSERVO_EVENT_DIRECTIONS:   
        LOG_DEBUG("Scan event received");
        (void)util_fsm_transition(fsm, servo_scan_state);
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("IDLE exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void servo_scan_state(FSM* fsm, Event* event)
{
    ServoObject* aobj = (ServoObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("SCAN entry");
        if(servo_set_both_angles(aobj, servo_scan_state_angles))
        {
            LOG_ERROR("Failed to set servos' angles");
        }
        break;
    case eSERVO_EVENT_DIRECTIONS:
        LOG_DEBUG("Directions event received");
        servo_scan_operation();
        if(servo_set_both_angles(aobj, servo_scan_state_angles))
        {
            LOG_ERROR("Failed to set servos' angles");
            (void)servo_get_angle(eSERVO_HORIZONTAL_CHANNEL, &servo_scan_state_angles.hor_angle);
            (void)servo_get_angle(eSERVO_VERTICAL_CHANNEL, &servo_scan_state_angles.ver_angle);
        }
        break;
    case eSERVO_EVENT_NOISE_DETECTED:
        LOG_DEBUG("Noise-detected event received");
        (void)util_fsm_transition(fsm, servo_noise_scan_state);
        break;
    case eSERVO_EVENT_LOCK:
        LOG_DEBUG("Lock event received");
        (void)util_fsm_transition(fsm, servo_target_lock_state);
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type); 
    }
}

void servo_noise_scan_state(FSM* fsm, Event* event)
{
    ServoObject* aobj = (ServoObject*)fsm->arg;
    ServoTarget target_copy;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("NOISE_SCAN entry");
        servo_copy_target(&target_copy);
        servo_scan_state_angles = target_copy.angles;
        if(servo_set_both_angles(aobj, target_copy.angles))
        {
            LOG_ERROR("Failed to set servos' angles");
        }
        (void)osal_timer_arm(aobj->timer, SERVO_MAX_ROTATION_DURATION_MS, eTIMER_TYPE_ONCE);
        break;
    case eSERVO_EVENT_DIRECTIONS:
        // Nothing to do in with this event. We wait on eSERVO_EVENT_ROTATION_TIMEOUT
        // to assure the servos finished rotating.
        break;
    case eSERVO_EVENT_ROTATION_TIMEOUT:
        LOG_DEBUG("Rotation timeout event received");
        (void)util_fsm_transition(fsm, servo_scan_state);
        break;
    case eSERVO_EVENT_NOISE_DETECTED:
        LOG_DEBUG("Noise-detected event received");
        // For now we don't handle this event from this state.
        // May change later.
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("NOISE_SCAN exit");
        (void)osal_timer_disarm(aobj->timer);
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type); 
    }
}

void servo_target_lock_state(FSM* fsm, Event* event)
{
    ServoObject* aobj = (ServoObject*)fsm->arg;
    static uint32_t last_seq;
    ServoTarget target_copy;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("TARGET_LOCK entry");
        servo_copy_target(&target_copy);
        last_seq = target_copy.seq;
        if(servo_set_both_angles(aobj, target_copy.angles))
        {
            LOG_ERROR("Failed to set servos' angles");
        }
        break;
    case eSERVO_EVENT_DIRECTIONS:
        LOG_DEBUG("Directions event received");
        servo_copy_target(&target_copy);
        if(last_seq == target_copy.seq)
            break;
        last_seq = target_copy.seq;
        if(servo_set_both_angles(aobj, target_copy.angles))
        {
            LOG_ERROR("Failed to set servos' angles");
        }
        break;
    case eSERVO_EVENT_SCAN: // should be sent by the `unlock` function in the application
        LOG_DEBUG("Scan event received");
        (void)util_fsm_transition(fsm, servo_scan_state);
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("TARGET_LOCK exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type); 
    }
}

#if 0
int main(void)
{
    printf("PCA9685 experiment: addr=0x%02X, PWM=%.0f Hz\n",
           eSERVO_PCA_ADDRESS, PWM_FREQ_HZ);

    if (pca9685_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "PCA9685 init failed\n");
        return 1;
    }
    printf("PCA9685 initialised.\n");

    float angle = 0.0f;

    while(angle <= 180.0f)
    {
        printf("\n--> Commanding both servos to %.1f deg\n", angle);

        if (servo_set_angle(eSERVO_CHANNEL1, angle) != eSTATUS_SUCCESSFUL)
            fprintf(stderr, "    servo1 (ch %d) set failed\n", eSERVO_CHANNEL1);
        if (servo_set_angle(eSERVO_CHANNEL2, angle) != eSTATUS_SUCCESSFUL)
            fprintf(stderr, "    servo2 (ch %d) set failed\n", eSERVO_CHANNEL2);

        /* Let the servos physically swing before we check. 500 ms is plenty
         * for a 9g servo over the whole 180 deg sweep. */
        sleep_us(500 * 1000);

        float reported = 0.0f;
        if (servo_get_angle(eSERVO_CHANNEL1, &reported) == eSTATUS_SUCCESSFUL)
            printf("    servo1 read-back: %.1f deg\n", reported);
        if (servo_get_angle(eSERVO_CHANNEL2, &reported) == eSTATUS_SUCCESSFUL)
            printf("    servo2 read-back: %.1f deg\n", reported);

        sleep_us(500 * 1000);
        angle += 10.0f;
    }

    printf("Reseting servos back to 0 degrees\n");
    if (servo_set_angle(eSERVO_CHANNEL1, 0.0f) != eSTATUS_SUCCESSFUL)
        printf("    servo1 (ch %d) set failed\n", eSERVO_CHANNEL1);
    if (servo_set_angle(eSERVO_CHANNEL2, 0.0f) != eSTATUS_SUCCESSFUL)
        printf("    servo2 (ch %d) set failed\n", eSERVO_CHANNEL2);
    printf("Done reseting servos\n");

    sleep_us(500 * 1000);

    /*  Park the chip in SLEEP on the way out so the servos stop twitching.
        We can remove this line if we want the servos to keep "holding weight". */
    (void)pca9685_write8(REG_MODE1, MODE1_SLEEP | MODE1_ALLCALL);

    hal_i2c_cleanup();
    printf("\nDone.\n");
    return 0;
}
#endif
