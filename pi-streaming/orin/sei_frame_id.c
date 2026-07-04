/**
 * sei_frame_id.c
 *
 * H.265 user-data-unregistered SEI carrier for the frame_id. See the header
 * for the rationale and wire format.
 *
 * NAL header for a prefix SEI (nal_unit_type 39, layer 0, temporal_id+1 = 1):
 *   byte0 = forbidden(0) | nal_unit_type(39)<<1 | (layer_id>>5) = 0x4E
 *   byte1 = (layer_id<<3) | temporal_id_plus1(1)               = 0x01
 */

#include "sei_frame_id.h"

#include <string.h>

#define HEVC_NAL_PREFIX_SEI   39
#define SEI_PT_USER_UNREG      5
#define SEI_FRAME_ID_PAYLOAD  20   /* 16-byte UUID + 4-byte id */

static const uint8_t k_uuid[16] = SEI_FRAME_ID_UUID;

int sei_frame_id_encode(uint32_t frame_id, uint8_t *out, size_t cap)
{
    if (out == NULL)
    {
        return -1;
    }

    /* Raw NAL: 2-byte header + payloadType + payloadSize + UUID + id + trailing. */
    uint8_t raw[2 + 1 + 1 + 16 + 4 + 1];
    size_t  n = 0;
    raw[n++] = 0x4E;                 /* NAL header: prefix SEI */
    raw[n++] = 0x01;
    raw[n++] = SEI_PT_USER_UNREG;    /* payloadType  = 5  */
    raw[n++] = SEI_FRAME_ID_PAYLOAD; /* payloadSize  = 20 */
    memcpy(raw + n, k_uuid, 16); n += 16;
    raw[n++] = (uint8_t)((frame_id >> 24) & 0xFF);
    raw[n++] = (uint8_t)((frame_id >> 16) & 0xFF);
    raw[n++] = (uint8_t)((frame_id >>  8) & 0xFF);
    raw[n++] = (uint8_t)( frame_id        & 0xFF);
    raw[n++] = 0x80;                 /* rbsp_trailing_bits */

    size_t o = 0;
    if (cap < 4)
    {
        return -1;
    }
    out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x01;  /* start code */

    /* Copy with emulation-prevention: a 0x03 is inserted before any byte
     * <= 0x03 that would otherwise follow two zero bytes. The NAL header has
     * no zero bytes, so running the scan over the whole NAL is equivalent to
     * scanning only the RBSP. */
    int zeros = 0;
    for (size_t i = 0; i < n; i++)
    {
        uint8_t b = raw[i];
        if (zeros >= 2 && b <= 0x03)
        {
            if (o >= cap) return -1;
            out[o++] = 0x03;
            zeros = 0;
        }
        if (o >= cap) return -1;
        out[o++] = b;
        zeros = (b == 0x00) ? (zeros + 1) : 0;
    }

    return (int)o;
}

/* Strip emulation-prevention bytes from a NAL body. Drops a 0x03 that follows
 * two zero bytes. Returns the de-EPB length written to out (out must be >= n). */
static size_t de_epb(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    int    zeros = 0;
    for (size_t i = 0; i < n; i++)
    {
        uint8_t b = in[i];
        if (b == 0x03 && zeros >= 2)
        {
            zeros = 0;       /* emulation-prevention byte: drop it */
            continue;
        }
        out[o++] = b;
        zeros = (b == 0x00) ? (zeros + 1) : 0;
    }
    return o;
}

/* Try to read our frame_id out of one de-EPB'd SEI RBSP (bytes after the
 * 2-byte NAL header). Returns true on a UUID match. */
static bool parse_sei_rbsp(const uint8_t *rbsp, size_t len, uint32_t *out)
{
    size_t p = 0;

    /* payloadType (ff_byte* + last_payload_type_byte) */
    int payload_type = 0;
    while (p < len && rbsp[p] == 0xFF) { payload_type += 255; p++; }
    if (p >= len) return false;
    payload_type += rbsp[p++];

    /* payloadSize */
    int payload_size = 0;
    while (p < len && rbsp[p] == 0xFF) { payload_size += 255; p++; }
    if (p >= len) return false;
    payload_size += rbsp[p++];

    if (payload_type != SEI_PT_USER_UNREG || payload_size < SEI_FRAME_ID_PAYLOAD)
    {
        return false;
    }
    if (p + (size_t)SEI_FRAME_ID_PAYLOAD > len)
    {
        return false;
    }
    if (memcmp(rbsp + p, k_uuid, 16) != 0)
    {
        return false;
    }

    const uint8_t *id = rbsp + p + 16;
    *out = ((uint32_t)id[0] << 24) | ((uint32_t)id[1] << 16) |
           ((uint32_t)id[2] <<  8) |  (uint32_t)id[3];
    return true;
}

bool sei_frame_id_parse(const uint8_t *data, size_t len, uint32_t *frame_id_out)
{
    if (data == NULL || frame_id_out == NULL)
    {
        return false;
    }

    size_t i = 0;
    while (i + 3 <= len)
    {
        /* Locate a start code 0x000001 (covers the 4-byte form too). */
        if (!(data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01))
        {
            i++;
            continue;
        }

        size_t nal_start = i + 3;

        /* Find the next start code to bound this NAL. */
        size_t nal_end = len;
        size_t j = nal_start;
        while (j + 3 <= len)
        {
            if (data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x01)
            {
                nal_end = j;
                /* Trim a trailing zero that is part of a 4-byte start code. */
                if (nal_end > nal_start && data[nal_end - 1] == 0x00)
                {
                    nal_end--;
                }
                break;
            }
            j++;
        }

        /* NAL must hold at least the 2-byte header. */
        if (nal_end >= nal_start + 2)
        {
            uint8_t  byte0    = data[nal_start];
            int      nal_type = (byte0 >> 1) & 0x3F;
            if (nal_type == HEVC_NAL_PREFIX_SEI)
            {
                const uint8_t *body = data + nal_start + 2;
                size_t         blen = nal_end - (nal_start + 2);

                uint8_t scratch[SEI_FRAME_ID_MAX_BYTES + 16];
                if (blen <= sizeof(scratch))
                {
                    size_t rlen = de_epb(body, blen, scratch);
                    if (parse_sei_rbsp(scratch, rlen, frame_id_out))
                    {
                        return true;
                    }
                }
            }
        }

        i = (nal_end > nal_start) ? nal_end : nal_start + 1;
    }

    return false;
}
