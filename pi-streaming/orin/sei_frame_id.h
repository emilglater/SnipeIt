/**
 * sei_frame_id.h
 *
 * Carry a frame_id inside the H.265 bitstream as a user-data-unregistered SEI
 * NAL, and recover it on the other side. This is the "SEI option" for the
 * Pi<->Orin frame_id tagging decision (vs an RTP header extension).
 *
 * Why SEI (recommended for this link):
 *   - It lives in the elementary stream, so it survives RTP (de)packetisation,
 *     FFmpeg, and any RTSP relay between the Pi and the Orin. An RTP header
 *     extension would have to be preserved by every hop.
 *   - It is codec-standard (Rec. ITU-T H.265, user_data_unregistered,
 *     payloadType 5), so the Orin can read it with any HEVC parser.
 *   - This Pi (Pi 5) has no hardware encoder and no encoder-level hook for
 *     per-frame SEI, so the sender splices this NAL into the bitstream itself,
 *     on a GStreamer pad probe between x265enc and h265parse.
 *
 * Wire detail:
 *   Annex-B prefix-SEI NAL (nal_unit_type 39), payloadType 5
 *   (user_data_unregistered): 16-byte UUID identifying this scheme, followed
 *   by the frame_id as 4 bytes big-endian. Emulation-prevention bytes are
 *   applied on encode and removed on parse.
 *
 * The Orin side must agree on the UUID and the 4-byte big-endian frame_id; see
 * SEI_FRAME_ID_UUID below.
 */

#ifndef ORIN_SEI_FRAME_ID_H
#define ORIN_SEI_FRAME_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 16-byte UUID (uuid_iso_iec_11578) that tags our user-data SEI so the Orin
 * can distinguish it from any other user data in the stream. Both ends MUST
 * use this exact value. */
#define SEI_FRAME_ID_UUID                                       \
    { 0x53, 0x6e, 0x69, 0x70, 0x65, 0x49, 0x74, 0x46,           \
      0x72, 0x6d, 0x49, 0x44, 0x00, 0x00, 0x00, 0x01 }

/* Worst-case encoded size: start code (4) + NAL header (2) + payloadType (1) +
 * payloadSize (1) + UUID (16) + id (4) + trailing (1) = 29, plus up to ~one
 * emulation-prevention byte per 2 payload bytes. 48 is comfortably safe. */
#define SEI_FRAME_ID_MAX_BYTES 48

/**
 * sei_frame_id_encode - Build an Annex-B prefix-SEI NAL carrying frame_id.
 *
 * @frame_id: The id to embed.
 * @out:      Destination buffer.
 * @cap:      Capacity of out (use SEI_FRAME_ID_MAX_BYTES).
 *
 * Returns the number of bytes written (including the 4-byte start code), or
 * -1 if cap is too small. Prepend the result to the frame's access unit.
 */
int sei_frame_id_encode(uint32_t frame_id, uint8_t *out, size_t cap);

/**
 * sei_frame_id_parse - Find our SEI in an Annex-B stream and read frame_id.
 *
 * @data:           Annex-B byte stream (one or more NALs).
 * @len:            Length of data.
 * @frame_id_out:   Filled with the recovered id on success.
 *
 * Returns true if a matching SEI (correct UUID) was found, false otherwise.
 * Scans all NALs and returns the first match. Mirrors what the Orin does.
 */
bool sei_frame_id_parse(const uint8_t *data, size_t len, uint32_t *frame_id_out);

#endif /* ORIN_SEI_FRAME_ID_H */
