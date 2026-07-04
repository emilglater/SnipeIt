/*
 * Experiment: drive two DFROBOT SER0049 servos via a PCA9685 over I2C,
 * and read back each servo's last-commanded angle.
 *
 * The servo itself has no position feedback, but the PCA9685 is a
 * register-based chip whose LEDn_ON / LEDn_OFF registers persist whatever
 * we last wrote into them (datasheet §7.3.3). We use those as the single
 * source of truth for "what angle did we last command".
 *
 * Wiring (Raspberry Pi 5 -> PCA9685):
 *   Pi pin 3 (SDA1) -> PCA9685 SDA
 *   Pi pin 5 (SCL1) -> PCA9685 SCL
 *   PCA9685 A0..A5 all tied LOW       -> 7-bit slave address 0x40
 *   PCA9685 LED0 output               -> SER0049 #1 signal pin
 *   PCA9685 LED1 output               -> SER0049 #2 signal pin
 *   Servo V+ from a separate 5-6V supply (NOT the Pi's 3.3V rail).
 *
 * References:
 *   - PCA9685 datasheet (rev.4, 2015-04-16) sections 7.3.1, 7.3.3, 7.3.5
 *   - Adafruit_PWMServoDriver.cpp (sleep -> prescale -> wake sequence)
 *   - SER0049 product page: pulse range 500..2500 us, sweep ~180 deg
 *  to compile this file run on this folders terminal :  gcc -I../src -o i2c_signal i2c_signal.c ../src/hal/i2c/hal_i2c.c 
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "../src/hal/i2c/hal_i2c.h"   /* transitively pulls in status.h */

/* ============================== Configuration ============================ */

/* Index of the PCA9685 in the HAL's I2C device table (same slot the
 * previous experiment used). */
#define I2C_DEVICE_INDEX        0

/* 7-bit I2C slave address. With A0..A5 all tied LOW this is 0x40. */
#define PCA9685_ADDRESS         0x40

/* PWM refresh rate for all 16 channels. Standard hobby servos want ~50 Hz.
 * PCA9685 allows 24..1526 Hz; SER0049 is happy anywhere in 50..60 Hz. */
#define PWM_FREQ_HZ             50.0f

/* SER0049 spec: 500..2500 us pulse maps to 0..180 degrees. */
#define SERVO_MIN_PULSE_US      500.0f
#define SERVO_MAX_PULSE_US      2500.0f
#define SERVO_MIN_ANGLE_DEG     0.0f
#define SERVO_MAX_ANGLE_DEG     180.0f

/* Which PCA9685 channel each servo is wired to. */
#define SERVO1_CHANNEL          0
#define SERVO2_CHANNEL          1

/* ============================ PCA9685 constants ========================== */

/* Internal oscillator, used when EXTCLK pin is grounded (datasheet §7.3.5). */
#define PCA9685_OSC_HZ          25000000.0f

/* Number of PWM steps per refresh cycle (12-bit counter: 0..4095). */
#define PCA9685_PWM_STEPS       4096

/* Datasheet forces a minimum prescale of 3. */
#define PCA9685_PRESCALE_MIN    3
#define PCA9685_PRESCALE_MAX    255

/* Register addresses */
#define REG_MODE1               0x00
#define REG_MODE2               0x01
#define REG_LED0_ON_L           0x06
#define REG_PRE_SCALE           0xFE

/* Each channel occupies 4 consecutive registers starting at REG_LED0_ON_L.
 * Channel n -> base address = 0x06 + 4*n. */
#define LEDn_BASE(channel)      (REG_LED0_ON_L + 4 * (channel))

/* MODE1 register bits */
#define MODE1_RESTART           0x80
#define MODE1_EXTCLK            0x40
#define MODE1_AI                0x20    /* auto-increment the register pointer */
#define MODE1_SLEEP             0x10
#define MODE1_ALLCALL           0x01

/* MODE2 register bits */
#define MODE2_INVRT             0x10
#define MODE2_OCH               0x08
#define MODE2_OUTDRV            0x04    /* 1 = totem-pole (what servos want) */

/* ================================ Helpers ================================ */

static void sleep_us(long us)
{
    struct timespec ts;
    ts.tv_sec  = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

static float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* ========================= PCA9685 register I/O ========================== */

static eStatus pca9685_write8(uint8_t reg, uint8_t value)
{
    return hal_i2c_write_reg(I2C_DEVICE_INDEX, reg, 1, &value, 1);
}

static eStatus pca9685_read8(uint8_t reg, uint8_t* out_value)
{
    if (out_value == NULL) return eSTATUS_NULL_PARAM;
    return hal_i2c_read_reg(I2C_DEVICE_INDEX, reg, 1, out_value, 1);
}

/*
 * Programs the PWM refresh rate. Per datasheet §7.3.5, the PRE_SCALE
 * register can only be written while the SLEEP bit of MODE1 is set, so
 * this function goes through the full sleep -> write -> wake -> restart
 * dance (matches what Adafruit's library does).
 */
static eStatus pca9685_set_pwm_freq(float freq_hz)
{
    /* Datasheet equation 1: prescale = round(oscillator_freq / (4096 * desired_freq)) - 1 */
    float   pre_f = (PCA9685_OSC_HZ / (PCA9685_PWM_STEPS * freq_hz)) - 1.0f;
    int     pre_i = (int)(pre_f + 0.5f);
    if (pre_i < PCA9685_PRESCALE_MIN)
        pre_i = PCA9685_PRESCALE_MIN;
    if (pre_i > PCA9685_PRESCALE_MAX)
        pre_i = PCA9685_PRESCALE_MAX;
    uint8_t prescale = (uint8_t)pre_i;

    uint8_t mode1_old;
    eStatus st = pca9685_read8(REG_MODE1, &mode1_old);
    if (st != eSTATUS_SUCCESSFUL) return st;

    /* Clear RESTART (writing 0 has no effect - it only self-clears on a
     * write of 1 - but we don't want to accidentally command a restart
     * mid-reconfig), force SLEEP=1 so the prescale write is accepted. */
    uint8_t mode1_sleep = (mode1_old & ~MODE1_RESTART) | MODE1_SLEEP;
    st = pca9685_write8(REG_MODE1, mode1_sleep);
    if (st != eSTATUS_SUCCESSFUL) return st;

    st = pca9685_write8(REG_PRE_SCALE, prescale);
    if (st != eSTATUS_SUCCESSFUL) return st;

    /* Restore original MODE1 (clears SLEEP if it wasn't set originally). */
    st = pca9685_write8(REG_MODE1, mode1_old);
    if (st != eSTATUS_SUCCESSFUL) return st;

    /* Oscillator can take up to 500 us to stabilise (datasheet §7.3.1 n.2). */
    sleep_us(500);

    /* Set RESTART (resumes any channels that were in sleep) and AI
     * (auto-increment) on top of the original mode bits. */
    return pca9685_write8(REG_MODE1,
                          (uint8_t)(mode1_old | MODE1_RESTART | MODE1_AI));
}

/*
 * One-shot PCA9685 bring-up:
 *   - opens the I2C bus (hal_i2c_init)
 *   - sets the 7-bit slave address
 *   - configures MODE2 = totem-pole outputs (servos need a real high level)
 *   - wakes the oscillator
 *   - programs the PWM refresh rate from PWM_FREQ_HZ
 *   - leaves AI=1 so per-channel 4-byte bursts work
 */
static eStatus pca9685_init(void)
{
    eStatus st = hal_i2c_init();
    if (st != eSTATUS_SUCCESSFUL) return st;

    st = hal_i2c_set_address(I2C_DEVICE_INDEX, PCA9685_ADDRESS);
    if (st != eSTATUS_SUCCESSFUL) return st;

    /* MODE2: totem-pole, non-inverting, change-on-STOP (all default-ish
     * except OUTDRV which we make explicit). */
    st = pca9685_write8(REG_MODE2, MODE2_OUTDRV);
    if (st != eSTATUS_SUCCESSFUL) return st;

    /* MODE1: clear SLEEP so oscillator runs, keep ALLCALL for compatibility.
     * AI and RESTART get set inside pca9685_set_pwm_freq. */
    st = pca9685_write8(REG_MODE1, MODE1_ALLCALL);
    if (st != eSTATUS_SUCCESSFUL) return st;

    sleep_us(500);

    return pca9685_set_pwm_freq(PWM_FREQ_HZ);
}

/*
 * Writes the 12-bit ON/OFF counts for a single channel in one I2C burst,
 * using the AI (auto-increment) feature. Layout of the 4 data bytes matches
 * LEDn_ON_L, LEDn_ON_H, LEDn_OFF_L, LEDn_OFF_H.
 */
static eStatus pca9685_set_pwm(uint8_t channel, uint16_t on, uint16_t off)
{
    if (channel > 15) return eSTATUS_INVALID_VALUE;

    /* Mask to 12 bits; bit 4 of the H bytes is the full-ON / full-OFF
     * flag, which we deliberately leave as 0. */
    uint8_t data[4];
    data[0] = (uint8_t)( on        & 0xFF);
    data[1] = (uint8_t)((on  >> 8) & 0x0F);
    data[2] = (uint8_t)( off       & 0xFF);
    data[3] = (uint8_t)((off >> 8) & 0x0F);

    return hal_i2c_write_reg(I2C_DEVICE_INDEX,
                             LEDn_BASE(channel), 1,
                             data, sizeof(data));
}

/*
 * Reads the 4 channel bytes back and decodes them into 12-bit ON/OFF counts.
 * Either output pointer may be NULL if that half isn't needed.
 */
static eStatus pca9685_get_pwm(uint8_t channel,
                               uint16_t* out_on, uint16_t* out_off)
{
    if (channel > 15) return eSTATUS_INVALID_VALUE;

    uint8_t data[4] = {0};
    eStatus st = hal_i2c_read_reg(I2C_DEVICE_INDEX,
                                  LEDn_BASE(channel), 1,
                                  data, sizeof(data));
    if (st != eSTATUS_SUCCESSFUL) return st;

    if (out_on)  *out_on  = (uint16_t)(data[0] | ((data[1] & 0x0F) << 8));
    if (out_off) *out_off = (uint16_t)(data[2] | ((data[3] & 0x0F) << 8));
    return eSTATUS_SUCCESSFUL;
}

/* ============================ Servo-angle API ============================ */

/* Number of PWM counts per microsecond at the configured refresh rate. */
static float counts_per_us(void)
{
    float period_us = 1000000.0f / PWM_FREQ_HZ;
    return (float)PCA9685_PWM_STEPS / period_us;
}

static uint16_t pulse_us_to_counts(float pulse_us)
{
    float counts = pulse_us * counts_per_us();
    if (counts < 0.0f)                         counts = 0.0f;
    if (counts > (float)(PCA9685_PWM_STEPS-1)) counts = PCA9685_PWM_STEPS - 1;
    return (uint16_t)(counts + 0.5f);
}

static float counts_to_pulse_us(uint16_t counts)
{
    return (float)counts / counts_per_us();
}

/*
 * Commands the servo on @channel to @angle_deg (clamped to 0..180).
 * Leading edge of the pulse is anchored at count 0 (ON=0); pulse width
 * is encoded in the OFF count. This matches what the Adafruit library
 * does in its setPWM(n, 0, pulse) examples and what the datasheet
 * illustrates in figure 7/example 1.
 */
eStatus servo_set_angle(uint8_t channel, float angle_deg)
{
    angle_deg = clampf(angle_deg, SERVO_MIN_ANGLE_DEG, SERVO_MAX_ANGLE_DEG);

    float pulse_us =
        SERVO_MIN_PULSE_US +
        (angle_deg / (SERVO_MAX_ANGLE_DEG - SERVO_MIN_ANGLE_DEG)) *
        (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US);

    return pca9685_set_pwm(channel, 0, pulse_us_to_counts(pulse_us));
}

/*
 * Reads back whatever angle was last commanded for @channel by decoding
 * the PCA9685's OFF register. Returns eSTATUS_NULL_PARAM if out_angle_deg
 * is NULL. If the channel has never been written (POR state = full-OFF),
 * the returned angle is clamped to SERVO_MIN_ANGLE_DEG and is meaningless.
 */
eStatus servo_get_angle(uint8_t channel, float* out_angle_deg)
{
    if (out_angle_deg == NULL) return eSTATUS_NULL_PARAM;

    uint16_t off_counts = 0;
    eStatus  st = pca9685_get_pwm(channel, NULL, &off_counts);
    if (st != eSTATUS_SUCCESSFUL) return st;

    float pulse_us = counts_to_pulse_us(off_counts);
    float angle    =
        ((pulse_us - SERVO_MIN_PULSE_US) /
         (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US)) *
        (SERVO_MAX_ANGLE_DEG - SERVO_MIN_ANGLE_DEG);

    *out_angle_deg = clampf(angle, SERVO_MIN_ANGLE_DEG, SERVO_MAX_ANGLE_DEG);
    return eSTATUS_SUCCESSFUL;
}

/* ================================= main ================================= */

int main(void)
{
    printf("PCA9685 experiment: addr=0x%02X, PWM=%.0f Hz\n",
           PCA9685_ADDRESS, PWM_FREQ_HZ);

    if (pca9685_init() != eSTATUS_SUCCESSFUL)
    {
        fprintf(stderr, "PCA9685 init failed\n");
        return 1;
    }
    printf("PCA9685 initialised.\n");

    float angle = 90.0f;
#if 0
    while(angle <= 180.0f)
    {
        printf("\n--> Commanding both servos to %.1f deg\n", angle);

        if (servo_set_angle(SERVO1_CHANNEL, angle) != eSTATUS_SUCCESSFUL)
            fprintf(stderr, "    servo1 (ch %d) set failed\n", SERVO1_CHANNEL);
        if (servo_set_angle(SERVO2_CHANNEL, angle) != eSTATUS_SUCCESSFUL)
            fprintf(stderr, "    servo2 (ch %d) set failed\n", SERVO2_CHANNEL);

        /* Let the servos physically swing before we check. 500 ms is plenty
         * for a 9g servo over the whole 180 deg sweep. */
        sleep_us(500 * 1000);

        float reported = 0.0f;
        if (servo_get_angle(SERVO1_CHANNEL, &reported) == eSTATUS_SUCCESSFUL)
            printf("    servo1 read-back: %.1f deg\n", reported);
        if (servo_get_angle(SERVO2_CHANNEL, &reported) == eSTATUS_SUCCESSFUL)
            printf("    servo2 read-back: %.1f deg\n", reported);

        sleep_us(500 * 1000);
        angle += 10.0f;
    }
#endif

    printf("Reseting servos back to 0 degrees\n");
    if (servo_set_angle(SERVO1_CHANNEL, angle) != eSTATUS_SUCCESSFUL)
        printf("    servo1 (ch %d) set failed\n", SERVO1_CHANNEL);
    if (servo_set_angle(SERVO2_CHANNEL, angle) != eSTATUS_SUCCESSFUL)
        printf("    servo2 (ch %d) set failed\n", SERVO2_CHANNEL);
    printf("Done reseting servos\n");

    sleep_us(500 * 1000);

    /*  Park the chip in SLEEP on the way out so the servos stop twitching.
        We can remove this line if we want the servos to keep "holding weight". */
    //(void)pca9685_write8(REG_MODE1, MODE1_SLEEP | MODE1_ALLCALL);

    hal_i2c_cleanup();
    printf("\nDone.\n");
    return 0;
}
