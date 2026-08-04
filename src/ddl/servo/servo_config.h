#ifndef DDL_SERVO_CONFIG_H
#define DDL_SERVO_CONFIG_H

/* User library includes */
#include "hal/i2c/hal_i2c_config.h"

/* Register addresses */
#define REG_MODE1               0x00    /* Master control register */
#define REG_MODE2               0x01    /* Output configuration register */
#define REG_LED0_ON_L           0x06    /* Start of channel-specific registers*/
#define REG_PRE_SCALE           0xFE    /* PWM refresh rate register */

/* MODE1 register bits */
#define MODE1_RESTART           0x80    /* Self-clears when writing 1 to it*/
#define MODE1_EXTCLK            0x40
#define MODE1_AI                0x20 
#define MODE1_SLEEP             0x10
#define MODE1_ALLCALL           0x01

/* MODE2 register bits */
#define MODE2_INVRT             0x10
#define MODE2_OCH               0x08
#define MODE2_OUTDRV            0x04

/* Each channel occupies 4 consecutive registers starting at REG_LED0_ON_L.
 * Channel n -> base address = 0x06 + 4*n. */
#define LEDn_BASE(channel)      (REG_LED0_ON_L + 4 * (channel))

/* PWM refresh rate for all 16 channels. */
#define PWM_FREQ_HZ             50.0f

/* Internal oscillator, used when EXTCLK pin is grounded (datasheet §7.3.5). */
#define PCA9685_OSC_HZ          25000000.0f

/* Number of PWM steps per refresh cycle (12-bit counter: 0..4095). */
#define PCA9685_PWM_STEPS       4096

/* Datasheet forces a minimum prescale of 3. */
#define PCA9685_PRESCALE_MIN    3
#define PCA9685_PRESCALE_MAX    255

/* SER0049 spec: 500..2500 us pulse maps to 0..180 degrees. */
#define SERVO_MIN_PULSE_US      500.0f
#define SERVO_MAX_PULSE_US      2500.0f
#define SERVO_MIN_ANGLE_DEG     0.0f
#define SERVO_MAX_ANGLE_DEG     180.0f

#define SERVO_MAX_ROTATION_DURATION_MS  500

#define SERVO_STEP_ANGLE                9.0f
#define SERVO_HORIZONTAL_MIN_ANGLE_DEG  63.0f
#define SERVO_HORIZONTAL_MAX_ANGLE_DEG  117.0f
#define SERVO_VERTICAL_MIN_ANGLE_DEG    70.0f
#define SERVO_VERTICAL_MAX_ANGLE_DEG    150.0f

#define SERVO_DIFFERENCE_TOLERANCE  1.0f

#define SERVO_DECREASE_ANGLE    false
#define SERVO_INCREASE_ANGLE    true

#define SERVO_VERTICAL_ENABLE   false
#define SERVO_HORIZONTAL_ENABLE true

#define SERVO_COUNT_FOR_STEP    0

typedef enum eServoConfig
{
    eSERVO_QUEUE_CAPACITY       = 4,
    eSERVO_PCA_ADDRESS          = 0x40,
    eSERVO_HORIZONTAL_CHANNEL   = 0,
    eSERVO_VERTICAL_CHANNEL     = 1,
    eSERVO_I2C_DEVICE           = eI2C0_DEVICE
} eServoConfig;

#endif