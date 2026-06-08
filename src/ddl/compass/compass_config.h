#ifndef DDL_COMPASS_CONFIG_H
#define DDL_COMPASS_CONFIG_H

/* User library includes */
#include "hal/i2c/hal_i2c_config.h"

#define COMPASS_REG_XOUT_0           0x00    /* Xout[17:10]                 */
#define COMPASS_REG_XOUT_1           0x01    /* Xout[9:2]                   */
#define COMPASS_REG_YOUT_0           0x02    /* Yout[17:10]                 */
#define COMPASS_REG_YOUT_1           0x03    /* Yout[9:2]                   */
#define COMPASS_REG_ZOUT_0           0x04    /* Zout[17:10]                 */
#define COMPASS_REG_ZOUT_1           0x05    /* Zout[9:2]                   */
#define COMPASS_REG_XYZOUT_2         0x06    /* Xout/Yout/Zout [1:0]        */
#define COMPASS_REG_TOUT             0x07    /* Temperature output          */
#define COMPASS_REG_STATUS           0x08    /* Status                      */
#define COMPASS_REG_INTERNAL_CTRL_0  0x09    /* Internal Control 0          */
#define COMPASS_REG_INTERNAL_CTRL_1  0x0A    /* Internal Control 1          */
#define COMPASS_REG_INTERNAL_CTRL_2  0x0B    /* Internal Control 2          */
#define COMPASS_REG_INTERNAL_CTRL_3  0x0C    /* Internal Control 3          */
#define COMPASS_REG_PRODUCT_ID       0x2F    /* Product ID (reads 0x30)     */

/* Status bits */
#define COMPASS_STATUS_MEAS_M_DONE   0x01
#define COMPASS_STATUS_MEAS_T_DONE   0x02
#define COMPASS_STATUS_OTP_RD_DONE   0x10

/* Internal Control 0 bits */
#define COMPASS_CTRL0_TM_M           0x01    /* Trigger magnetic measurement    */
#define COMPASS_CTRL0_TM_T           0x02    /* Trigger temperature measurement */
#define COMPASS_CTRL0_SET            0x08    /* Fire SET coil pulse             */
#define COMPASS_CTRL0_RESET          0x10    /* Fire RESET coil pulse           */

/* Internal Control 1 bits */
#define COMPASS_CTRL1_BW_8MS         0x00    /* BW=00: 8ms / 100 Hz BW (best noise) */
#define COMPASS_CTRL1_SW_RST         0x80    /* Software reset                      */

/* Product ID register reads 0x30 at reset */
#define COMPASS_PRODUCT_ID_VALUE     0x30

/* Mid-scale of the 18-bit unsigned output ("Null Field Output"). A
 * reading equal to this value corresponds to zero magnetic field on
 * that axis. Used to convert offset-binary -> signed counts. */
#define COMPASS_18BIT_NULL_FIELD     131072U   /* = 2^17 */

/* Sensitivity at 18-bit resolution. */
#define COMPASS_18BIT_COUNTS_PER_GAUSS   16384.0f

/* Per-measurement time at BW=00 is 8 ms typical; we allow a small
 * margin for jitter / I2C overhead before polling times out. */
#define COMPASS_MEAS_TIMEOUT_MS      15

/* SET and RESET pulses themselves are 500 ns wide. The chip needs a
 * moment for the coil currents to settle before the next measurement.
 * A 1 ms wait is far more than the datasheet implies but it is also
 * negligible inside our ~333 ms scheduler slot. */
#define COMPASS_SET_RESET_SETTLE_MS  1

/* Software reset takes ~10 ms (datasheet, Internal Control 1 SW_RST). */
#define COMPASS_SW_RST_MS            10

/* Bound on how many times we re-poll Status.Meas_M_Done while waiting
 * for a measurement to complete. With a 1 ms poll interval and an
 * 8 ms measurement, MAGNET_MEAS_TIMEOUT_MS gives plenty of headroom. */
#define COMPASS_MEAS_POLL_INTERVAL_MS    1

/* Temperature output (datasheet page 14): unsigned 8-bit, 0.8 °C/LSB,
 * 0x00 maps to -75 °C. So temp_c = (raw * 0.8) - 75. */
#define COMPASS_TEMP_OFFSET_C        (-75.0f)
#define COMPASS_TEMP_SCALE_C_PER_LSB (0.8f)

/* Magnetic declination in degrees, added to the computed heading
 * before normalisation. Based on NOAA's magnetic calculator */
#define COMPASS_DECLINATION_DEG      4.5f

/* Additional fixed offset added to the heading */
#define COMPASS_HEADING_OFFSET_DEG   0.0f

/* 0 - +Z axis is down; 1 - +Z is up */
#define COMPASS_INVERT_HEADING       1

/* Pitch / roll compensation degrees */
#define COMPASS_DEFAULT_PITCH_DEG    0.0f
#define COMPASS_DEFAULT_ROLL_DEG     0.0f

typedef enum eCompassConfig
{
    eCOMPASS_QUEUE_CAPACITY  = 4,
    eCOMPASS_RETRY_MAX       = 3,
    eCOMPASS_I2C_ADDRESS     = 0x30,         /* 7-bit, fixed */
    eCOMPASS_I2C_DEVICE      = eI2C1_DEVICE
} eCompassConfig;

#endif