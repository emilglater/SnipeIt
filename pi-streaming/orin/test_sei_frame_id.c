/**
 * test_sei_frame_id.c
 *
 * Unit test for the H.265 SEI frame_id carrier. Pure bytes, no camera/encoder.
 * Build/run:  make test_sei_frame_id && ./test_sei_frame_id
 */

#include "sei_frame_id.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg,             \
                    __FILE__, __LINE__);                           \
            g_failures++;                                           \
        }                                                          \
    } while (0)

/* Round-trip a single id through encode -> parse. */
static void roundtrip(uint32_t id)
{
    uint8_t buf[SEI_FRAME_ID_MAX_BYTES];
    int n = sei_frame_id_encode(id, buf, sizeof(buf));
    CHECK(n > 0, "encode succeeds");

    /* Structural checks: Annex-B start code + prefix-SEI NAL type. */
    CHECK(n >= 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1,
          "4-byte start code");
    CHECK(((buf[4] >> 1) & 0x3F) == 39, "NAL type 39 (prefix SEI)");

    uint32_t got = 0xDEADBEEF;
    CHECK(sei_frame_id_parse(buf, (size_t)n, &got), "parse finds SEI");
    if (got != id)
    {
        fprintf(stderr, "FAIL: roundtrip id 0x%08X got 0x%08X\n", id, got);
        g_failures++;
    }
}

static void test_roundtrips(void)
{
    roundtrip(0);
    roundtrip(1);
    roundtrip(42);
    roundtrip(12345);
    roundtrip(0xFFFFFFFF);
    /* Values whose bytes force emulation-prevention (00 00 0x sequences). */
    roundtrip(0x00000001);
    roundtrip(0x00000003);
    roundtrip(0x00000100);
    roundtrip(0x00010002);
    roundtrip(0x01000000);
}

/* The SEI must be found when embedded between other NAL units, as it will be
 * in a real access unit (VPS/SPS/PPS/slice around it). */
static void test_embedded_in_stream(void)
{
    uint8_t stream[256];
    size_t  o = 0;

    /* Fake VPS NAL (type 32): start code + header + a few bytes. */
    const uint8_t vps[] = { 0x00,0x00,0x00,0x01, 0x40,0x01, 0x0C,0x01,0xFF };
    memcpy(stream + o, vps, sizeof(vps)); o += sizeof(vps);

    /* Our SEI. */
    int n = sei_frame_id_encode(0x0BADF00D & 0xFFFF, stream + o, sizeof(stream) - o);
    CHECK(n > 0, "embed encode");
    o += (size_t)n;

    /* Fake slice NAL (type 1): start code + header + payload incl zeros. */
    const uint8_t slice[] = { 0x00,0x00,0x00,0x01, 0x02,0x01, 0xAB,0x00,0x00,0xCD };
    memcpy(stream + o, slice, sizeof(slice)); o += sizeof(slice);

    uint32_t got = 0;
    CHECK(sei_frame_id_parse(stream, o, &got), "parse finds SEI in stream");
    CHECK(got == (0x0BADF00D & 0xFFFF), "embedded id correct");
}

static void test_no_sei(void)
{
    /* A stream with VPS + slice but no user SEI -> no match. */
    const uint8_t stream[] = {
        0x00,0x00,0x00,0x01, 0x40,0x01, 0x0C,0x01,
        0x00,0x00,0x00,0x01, 0x02,0x01, 0xAB,0xCD,
    };
    uint32_t got = 0;
    CHECK(!sei_frame_id_parse(stream, sizeof(stream), &got), "no SEI -> false");
}

static void test_bad_args(void)
{
    uint8_t buf[SEI_FRAME_ID_MAX_BYTES];
    CHECK(sei_frame_id_encode(1, NULL, sizeof(buf)) < 0, "NULL out rejected");
    CHECK(sei_frame_id_encode(1, buf, 3) < 0, "tiny buffer rejected");

    uint32_t got;
    CHECK(!sei_frame_id_parse(NULL, 10, &got), "NULL data rejected");
    CHECK(!sei_frame_id_parse(buf, 0, &got), "empty data -> false");
}

int main(void)
{
    test_roundtrips();
    test_embedded_in_stream();
    test_no_sei();
    test_bad_args();

    if (g_failures == 0)
    {
        printf("PASS: all sei_frame_id tests\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
}
