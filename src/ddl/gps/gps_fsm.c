#include "gps_fsm.h"

/* Standard library includes */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* User library includes */
#include "ddl/gps/gps_config.h"
#include "ddl/gps/gps_types.h"
#include "hal/uart/hal_uart.h"
#include "util/log/log.h"
#include "osal/osal.h"

/* UBX frame layout (u-blox proprietary binary protocol).
 *
 *   B5 62 | <class> | <id> | <length lo> <length hi> | <payload...> | <CK_A> <CK_B>
 *
 * The two-byte length field is the PAYLOAD length only — it excludes
 * the 6-byte header and the 2-byte checksum. Multi-byte fields are
 * little-endian on the wire. The Fletcher-style 8-bit checksum runs
 * over (class + id + length + payload), so the two sync bytes are
 * deliberately skipped. */
typedef enum eUbxFrameFields
{
    eUBX_SYNC_1          = 0xB5,
    eUBX_SYNC_2          = 0x62,

    /* NAV class */
    eUBX_CLS_NAV         = 0x01,
    eUBX_ID_NAV_PVT      = 0x07,
    eUBX_PVT_PAYLOAD_LEN = 92,

    /* CFG class */
    eUBX_CLS_CFG         = 0x06,
    eUBX_ID_CFG_MSG      = 0x01,
    eUBX_ID_CFG_RATE     = 0x08,

    /* NMEA standard message class (used as the msgClass field of
     * UBX-CFG-MSG when disabling NMEA streams). */
    eUBX_NMEA_CLASS      = 0xF0,
    eUBX_NMEA_GGA        = 0x00,
    eUBX_NMEA_GLL        = 0x01,
    eUBX_NMEA_GSA        = 0x02,
    eUBX_NMEA_GSV        = 0x03,
    eUBX_NMEA_RMC        = 0x04,
    eUBX_NMEA_VTG        = 0x05,
    eUBX_NMEA_TXT        = 0x41
} eUbxFrameFields;

typedef struct __attribute__((packed))
{
    /* UBX header (6 bytes) */
    uint8_t  sync1;
    uint8_t  sync2;
    uint8_t  msg_class;
    uint8_t  msg_id;
    uint16_t payload_length;

    /* UBX-NAV-PVT payload (92 bytes) */
    uint32_t i_tow;        /* GPS time of week of nav epoch [ms] */
    uint16_t year;         /* UTC year */
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  min;
    uint8_t  sec;
    uint8_t  valid_flags;  /* bit0=date, bit1=time, bit2=fully resolved, bit3=mag */
    uint32_t t_acc;        /* time accuracy estimate [ns] */
    int32_t  nano;         /* time accuracy estimate: fraction of second [ns], -1e9..+1e9 */
    uint8_t  fix_type;     /* Quality of the fix: 0=no fix, 1=DR, 2=2D, 3=3D, 4=GNSS+DR, 5=time-only */
    uint8_t  flags;
    uint8_t  flags2;
    uint8_t  num_sv;       /* number of satellites used in solution */
    int32_t  lon;          /* longitude [deg * 1e-7] */
    int32_t  lat;          /* latitude  [deg * 1e-7] */
    int32_t  height;       /* height above ellipsoid [mm] */
    int32_t  height_msl;   /* height above mean sea level [mm] */
    uint32_t h_acc;        /* horizontal accuracy estimate [mm] */
    uint32_t v_acc;        /* vertical accuracy estimate [mm] */
    int32_t  vel_n;        /* NED north velocity [mm/s] */
    int32_t  vel_e;        /* NED east  velocity [mm/s] */
    int32_t  vel_d;        /* NED down  velocity [mm/s] */
    int32_t  g_speed;      /* ground speed (2-D) [mm/s] */
    int32_t  head_mot;     /* heading of motion (2-D) [deg * 1e-5] */
    uint32_t s_acc;        /* speed accuracy estimate [mm/s] */
    uint32_t head_acc;     /* heading accuracy estimate [deg * 1e-5] */
    uint16_t p_dop;        /* position DOP * 0.01 */
    uint8_t  flags3;
    uint8_t  reserved[5];
    int32_t  head_veh;     /* heading of vehicle (2-D) [deg * 1e-5] */
    int16_t  mag_dec;      /* magnetic declination [deg * 1e-2] */
    uint16_t mag_acc;      /* declination accuracy [deg * 1e-2] */

    /* UBX trailer (2 bytes) */
    uint8_t  checksum_a;
    uint8_t  checksum_b;
} UbxNavPvtFrame;

typedef struct __attribute__((packed))
{
    uint8_t  sync1;
    uint8_t  sync2;
    uint8_t  msg_class;
    uint8_t  msg_id;
    uint16_t payload_length;
    uint8_t  checksum_a;
    uint8_t  checksum_b;
} UbxReadCmd;

typedef struct
{
    uint8_t msg_class;
    uint8_t msg_id;
    uint8_t payload_length;
    uint8_t payload[16];
} ConfigStep;

static TimerArg timer_arg;

static UbxNavPvtFrame   resp_frame;

static uint8_t  config_transmit_buf[24];

static uint32_t config_step;

static const UbxReadCmd read_cmd = {
    .sync1          = eUBX_SYNC_1,
    .sync2          = eUBX_SYNC_2,
    .msg_class      = eUBX_CLS_NAV,
    .msg_id         = eUBX_ID_NAV_PVT,
    .payload_length = 0,
    .checksum_a     = 0x08,
    .checksum_b     = 0x19
};

static const ConfigStep config_sequence[] = {
    { eUBX_CLS_CFG, eUBX_ID_CFG_MSG, 3, { eUBX_NMEA_CLASS, eUBX_NMEA_GGA, 0x00 } },
    { eUBX_CLS_CFG, eUBX_ID_CFG_MSG, 3, { eUBX_NMEA_CLASS, eUBX_NMEA_GLL, 0x00 } },
    { eUBX_CLS_CFG, eUBX_ID_CFG_MSG, 3, { eUBX_NMEA_CLASS, eUBX_NMEA_GSA, 0x00 } },
    { eUBX_CLS_CFG, eUBX_ID_CFG_MSG, 3, { eUBX_NMEA_CLASS, eUBX_NMEA_GSV, 0x00 } },
    { eUBX_CLS_CFG, eUBX_ID_CFG_MSG, 3, { eUBX_NMEA_CLASS, eUBX_NMEA_RMC, 0x00 } },
    { eUBX_CLS_CFG, eUBX_ID_CFG_MSG, 3, { eUBX_NMEA_CLASS, eUBX_NMEA_VTG, 0x00 } },
    { eUBX_CLS_CFG, eUBX_ID_CFG_MSG, 3, { eUBX_NMEA_CLASS, eUBX_NMEA_TXT, 0x00 } },
    { eUBX_CLS_CFG, eUBX_ID_CFG_RATE, 6,
      { 0xE8, 0x03,   /* measRate = 1000 ms (1 Hz)            */
        0x01, 0x00,   /* navRate  = 1 measurement per nav epoch */
        0x01, 0x00 }  /* timeRef  = GPS                         */
    }
};

#define CFG_SEQ_LEN (sizeof(config_sequence) / sizeof(config_sequence[0]))

static void ubx_checksum(const uint8_t* buf, uint32_t len,
                         uint8_t* out_checksum_a, uint8_t *out_checksum_b)
{
    uint8_t checksum_a = 0;
    uint8_t checksum_b = 0;

    for(uint32_t i = 0; i < len; i++)
    {
        checksum_a = (uint8_t)(checksum_a + buf[i]);
        checksum_b = (uint8_t)(checksum_b + checksum_a);
    }

    *out_checksum_a = checksum_a;
    *out_checksum_b = checksum_b;
}

static uint16_t from_little_endian16(uint16_t value)
{
    uint8_t *buf = (uint8_t *)&value;

    return (uint16_t)(((uint16_t)buf[0] << 0) |
                      ((uint16_t)buf[1] << 8));
}

static uint32_t from_little_endian32(uint32_t value)
{
    uint8_t *buf = (uint8_t *)&value;

    return ((uint32_t)buf[0] << 0)  |
           ((uint32_t)buf[1] << 8)  |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static int32_t from_little_endian_i32(int32_t value)
{
    return (int32_t)from_little_endian32((uint32_t)value);
}

static bool is_frame_valid(const UbxNavPvtFrame* frame, uint32_t old_i_tow)
{
    uint8_t expected_checksum_a;
    uint8_t expected_checksum_b;

    if(frame->sync1 != eUBX_SYNC_1 || frame->sync2 != eUBX_SYNC_2)
    {
        return false;
    }

    if(frame->msg_class != eUBX_CLS_NAV || frame->msg_id != eUBX_ID_NAV_PVT)
    {
        return false;
    }

    if(from_little_endian16(frame->payload_length) != eUBX_PVT_PAYLOAD_LEN)
    {
        return false;
    }

    /* Stale-response guard */
    if(from_little_endian32(frame->i_tow) == old_i_tow)
    {
        return false;
    }

    /* Checksum covers everything between the sync bytes and the two
     * checksum bytes: cls + id + length + payload = 96 bytes. */
    ubx_checksum(((const uint8_t *)frame) + 2,
                 sizeof(UbxNavPvtFrame) - 4,
                 &expected_checksum_a, &expected_checksum_b);
    if(frame->checksum_a != expected_checksum_a || frame->checksum_b != expected_checksum_b)
    {
        return false;
    }

    return true;
}

static void update_gps_frame(GPSObject* aobj, const UbxNavPvtFrame* frame)
{
    aobj->frame->valid = true;

    aobj->frame->latitude = (double)from_little_endian_i32(frame->lat) * 1e-7;
    aobj->frame->longitude = (double)from_little_endian_i32(frame->lon) * 1e-7;
    aobj->frame->altitude = (float)from_little_endian_i32(frame->height_msl) / 1000.0f;

    aobj->frame->fix_type = frame->fix_type;
    aobj->frame->num_satellites = frame->num_sv;
    aobj->frame->h_acc = (float)from_little_endian32(frame->h_acc) / 1000.0f;

    aobj->system_time = from_little_endian32(frame->i_tow);
}

static void config_step_done_handler(void* arg)
{
    static Event step_done_event = { .type = eGPS_EVENT_CONFIGURED };
    GPSObject* aobj = (GPSObject*)arg;
    (void)util_active_object_post(&aobj->aobj, &step_done_event);
}

/* Build the UBX frame for config_sequence[config_step] into config_transmit_buf and
 * fire it off. When config_step is past the end of the sequence, invoke
 * the user's "configuration done" callback instead. */
static void config_send_current_step(GPSObject* aobj)
{
    const ConfigStep*   step;

    uint32_t    frame_len;
    uint8_t     checksum_a;
    uint8_t     checksum_b;

    /*if(config_step >= CFG_SEQ_LEN)
    {
        LOG_DEBUG("Configuration sequence complete");
        if(config_user_cb != NULL)
        {
            config_user_cb(config_user_arg);
        }
        return;
    }*/

    step = &config_sequence[config_step];

    config_transmit_buf[0] = eUBX_SYNC_1;
    config_transmit_buf[1] = eUBX_SYNC_2;
    config_transmit_buf[2] = step->msg_class;
    config_transmit_buf[3] = step->msg_id;
    config_transmit_buf[4] = step->payload_length;  /* length lo (payload < 256) */
    config_transmit_buf[5] = 0x00;                  /* length hi */
    for(uint8_t i = 0; i < step->payload_length; i++)
    {
        config_transmit_buf[6 + i] = step->payload[i];
    }

    ubx_checksum(&config_transmit_buf[2], (uint32_t)(4 + step->payload_length),
                    &checksum_a, &checksum_b);
    config_transmit_buf[6 + step->payload_length] = checksum_a;
    config_transmit_buf[6 + step->payload_length + 1] = checksum_b;

    frame_len = (uint32_t)(6 + step->payload_length + 2);
    LOG_DEBUG("Sending config step %u (%u bytes)", config_step, frame_len);
    (void)hal_uart_write(eGPS_UART_DEVICE, config_transmit_buf, frame_len,
                            config_step_done_handler, aobj);
}

static void retry_handler(GPSObject* aobj, FSM* fsm)
{
    aobj->retry++;
    if(aobj->retry < eGPS_READ_RETRY_MAX)
    {
        (void)util_fsm_transition(fsm, gps_read_state);
    }
    else
    {
        LOG_DEBUG("Retries exceeded limit (%u)", aobj->retry);
        aobj->frame->valid = false;
        (void)util_fsm_transition(fsm, gps_idle_state);
    }
}

static void timeout_handler(void* arg)
{
    static Event timeout_event = { .type = eGPS_EVENT_TIMEOUT };
    GPSObject* aobj = (GPSObject*)arg;
    (void)util_active_object_post(&aobj->aobj, &timeout_event);
}

static void uart_rx_complete_handler(void* arg)
{
    static Event frame_received_event = { .type = eGPS_EVENT_FRAME_RECEIVED };
    GPSObject* aobj = (GPSObject*)arg;
    (void)util_active_object_post(&aobj->aobj, &frame_received_event);
}

void gps_init_state(FSM* fsm, Event* event)
{
    GPSObject* aobj = (GPSObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_INIT:
        LOG_DEBUG("INIT entry");
        aobj->frame->valid = false;
        timer_arg.handler = timeout_handler;
        timer_arg.arg = aobj;
        if(osal_timer_init(&aobj->timer, &timer_arg))
        {
            LOG_ERROR("Timer initialization failed");
            (void)util_fsm_transition(fsm, gps_error_state);
        }
        else
        {
            config_step = 0;
            config_send_current_step(aobj);
        }
        break;
    case eGPS_EVENT_CONFIGURED:
        LOG_DEBUG("Configuration step %u completed", config_step);
        config_step++;
        if(config_step < CFG_SEQ_LEN)
        {
            config_send_current_step(aobj);
        }
        else
        {
            LOG_DEBUG("Configuration complete");
            (void)hal_uart_flush_input(eGPS_UART_DEVICE);
            (void)util_fsm_transition(fsm, gps_idle_state);
        }
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("INIT exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void gps_error_state(FSM* fsm, Event* event)
{
    GPSObject* aobj = (GPSObject*)fsm->arg;

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

void gps_idle_state(FSM* fsm, Event* event)
{
    GPSObject* aobj = (GPSObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("IDLE entry");
        aobj->retry = 0;
        break;
    case eGPS_EVENT_READ:
        LOG_DEBUG("Read event received");
        (void)util_fsm_transition(fsm, gps_read_state);
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("IDLE exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}

void gps_read_state(FSM* fsm, Event* event)
{
    GPSObject* aobj = (GPSObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("READ entry");
        (void)hal_uart_write(eGPS_UART_DEVICE, &read_cmd, sizeof(read_cmd), NULL, NULL);
        (void)hal_uart_read(eGPS_UART_DEVICE, &resp_frame, sizeof(resp_frame), uart_rx_complete_handler, aobj);
        (void)osal_timer_arm(aobj->timer, eGPS_READ_TIMEOUT_MS, eTIMER_TYPE_ONCE);
        break;
    case eGPS_EVENT_FRAME_RECEIVED:
        LOG_DEBUG("Frame received");
        (void)util_fsm_transition(fsm, gps_update_state);
        break;
    case eGPS_EVENT_TIMEOUT:
        LOG_DEBUG("Read timed out");
        (void)hal_uart_abort(eGPS_UART_DEVICE);
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

void gps_update_state(FSM* fsm, Event* event)
{
    GPSObject* aobj = (GPSObject*)fsm->arg;

    switch(event->type)
    {
    case eFSM_EVENT_ENTRY:
        LOG_DEBUG("UPDATE entry");
        if(is_frame_valid(&resp_frame, aobj->system_time))
        {
            update_gps_frame(aobj, &resp_frame);
            LOG_DEBUG("Frame is valid. latitude=%f, logitude=%f, altitude=%f, satellites=%u",
                        aobj->frame->latitude, aobj->frame->longitude,
                        (double)aobj->frame->altitude, aobj->frame->num_satellites);
            (void)util_fsm_transition(fsm, gps_idle_state);
        }
        else
        {
            //LOG_WARNING("Frame is invalid");
            const uint8_t *raw = (const uint8_t *)&resp_frame;
            (void)raw;
            LOG_WARNING("Frame is invalid. Hex[0..15]: "
                        "%02X %02X %02X %02X %02X %02X %02X %02X "
                        "%02X %02X %02X %02X %02X %02X %02X %02X",
                        raw[0],  raw[1],  raw[2],  raw[3],
                        raw[4],  raw[5],  raw[6],  raw[7],
                        raw[8],  raw[9],  raw[10], raw[11],
                        raw[12], raw[13], raw[14], raw[15]);
            LOG_WARNING("Frame is invalid. Hex[16..31]: "
                        "%02X %02X %02X %02X %02X %02X %02X %02X "
                        "%02X %02X %02X %02X %02X %02X %02X %02X",
                        raw[16], raw[17], raw[18], raw[19],
                        raw[20], raw[21], raw[22], raw[23],
                        raw[24], raw[25], raw[26], raw[27],
                        raw[28], raw[29], raw[30], raw[31]);
            LOG_WARNING("Frame is invalid. ASCII[0..31]: "
                        "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c"
                        "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c",
                        (raw[0]  >= 0x20 && raw[0]  < 0x7F) ? raw[0]  : '.',
                        (raw[1]  >= 0x20 && raw[1]  < 0x7F) ? raw[1]  : '.',
                        (raw[2]  >= 0x20 && raw[2]  < 0x7F) ? raw[2]  : '.',
                        (raw[3]  >= 0x20 && raw[3]  < 0x7F) ? raw[3]  : '.',
                        (raw[4]  >= 0x20 && raw[4]  < 0x7F) ? raw[4]  : '.',
                        (raw[5]  >= 0x20 && raw[5]  < 0x7F) ? raw[5]  : '.',
                        (raw[6]  >= 0x20 && raw[6]  < 0x7F) ? raw[6]  : '.',
                        (raw[7]  >= 0x20 && raw[7]  < 0x7F) ? raw[7]  : '.',
                        (raw[8]  >= 0x20 && raw[8]  < 0x7F) ? raw[8]  : '.',
                        (raw[9]  >= 0x20 && raw[9]  < 0x7F) ? raw[9]  : '.',
                        (raw[10] >= 0x20 && raw[10] < 0x7F) ? raw[10] : '.',
                        (raw[11] >= 0x20 && raw[11] < 0x7F) ? raw[11] : '.',
                        (raw[12] >= 0x20 && raw[12] < 0x7F) ? raw[12] : '.',
                        (raw[13] >= 0x20 && raw[13] < 0x7F) ? raw[13] : '.',
                        (raw[14] >= 0x20 && raw[14] < 0x7F) ? raw[14] : '.',
                        (raw[15] >= 0x20 && raw[15] < 0x7F) ? raw[15] : '.',
                        (raw[16] >= 0x20 && raw[16] < 0x7F) ? raw[16] : '.',
                        (raw[17] >= 0x20 && raw[17] < 0x7F) ? raw[17] : '.',
                        (raw[18] >= 0x20 && raw[18] < 0x7F) ? raw[18] : '.',
                        (raw[19] >= 0x20 && raw[19] < 0x7F) ? raw[19] : '.',
                        (raw[20] >= 0x20 && raw[20] < 0x7F) ? raw[20] : '.',
                        (raw[21] >= 0x20 && raw[21] < 0x7F) ? raw[21] : '.',
                        (raw[22] >= 0x20 && raw[22] < 0x7F) ? raw[22] : '.',
                        (raw[23] >= 0x20 && raw[23] < 0x7F) ? raw[23] : '.',
                        (raw[24] >= 0x20 && raw[24] < 0x7F) ? raw[24] : '.',
                        (raw[25] >= 0x20 && raw[25] < 0x7F) ? raw[25] : '.',
                        (raw[26] >= 0x20 && raw[26] < 0x7F) ? raw[26] : '.',
                        (raw[27] >= 0x20 && raw[27] < 0x7F) ? raw[27] : '.',
                        (raw[28] >= 0x20 && raw[28] < 0x7F) ? raw[28] : '.',
                        (raw[29] >= 0x20 && raw[29] < 0x7F) ? raw[29] : '.',
                        (raw[30] >= 0x20 && raw[30] < 0x7F) ? raw[30] : '.',
                        (raw[31] >= 0x20 && raw[31] < 0x7F) ? raw[31] : '.');
            retry_handler(aobj, fsm);
        }
        break;
    case eFSM_EVENT_EXIT:
        LOG_DEBUG("UPDATE exit");
        break;
    default:
        LOG_WARNING("Unknown event type %u", event->type);
    }
}