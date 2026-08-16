/**
 * test_detection_msg.c
 *
 * Standalone unit test for the Orin detection-message JSON parser.
 * No zmq, no network. Build/run:  make test_detection_msg && ./test_detection_msg
 */

#include "detection_msg.h"

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

static bool parse(const char *s, OrinDetectionMsg *m)
{
    return orin_detection_msg_parse(s, strlen(s), m);
}

static void test_canonical(void)
{
    const char *s =
        "{\"type\":\"target_detection\",\"frame_id\":12345,\"timestamp_ms\":678,"
        "\"detections\":[{\"id\":\"1\",\"class\":\"HUMAN\",\"confidence\":0.85,"
        "\"bbox\":{\"x\":100,\"y\":50,\"width\":200,\"height\":400}}]}";
    OrinDetectionMsg m;
    CHECK(parse(s, &m), "canonical parses");
    CHECK(m.has_frame_id && m.frame_id == 12345u, "frame_id");
    CHECK(m.timestamp_ms == 678u, "timestamp_ms");
    CHECK(m.num_detections == 1, "one detection");
    CHECK(strcmp(m.detections[0].target_id, "1") == 0, "id");
    CHECK(strcmp(m.detections[0].cls, "HUMAN") == 0, "class");
    CHECK(m.detections[0].confidence > 0.84f && m.detections[0].confidence < 0.86f, "confidence");
    CHECK(m.detections[0].bbox_x == 100, "bbox.x");
    CHECK(m.detections[0].bbox_y == 50, "bbox.y");
    CHECK(m.detections[0].bbox_w == 200, "bbox.width");
    CHECK(m.detections[0].bbox_h == 400, "bbox.height");
}

static void test_multiple_and_trailing_newline(void)
{
    const char *s =
        "{\"frame_id\":7,\"detections\":["
        "{\"id\":\"a\",\"class\":\"HUMAN\",\"confidence\":0.5,\"bbox\":{\"x\":1,\"y\":2,\"width\":3,\"height\":4}},"
        "{\"id\":\"b\",\"class\":\"DRONE\",\"confidence\":0.9,\"bbox\":{\"x\":5,\"y\":6,\"width\":7,\"height\":8}}"
        "]}\n";
    OrinDetectionMsg m;
    CHECK(parse(s, &m), "multi parses (with trailing newline)");
    CHECK(m.num_detections == 2, "two detections");
    CHECK(strcmp(m.detections[1].cls, "DRONE") == 0, "second class");
    CHECK(m.detections[1].bbox_h == 8, "second bbox.height");
}

static void test_missing_frame_id(void)
{
    const char *s = "{\"detections\":[]}";
    OrinDetectionMsg m;
    CHECK(parse(s, &m), "missing frame_id still parses");
    CHECK(!m.has_frame_id, "has_frame_id false");
    CHECK(m.num_detections == 0, "empty detections");
}

static void test_unknown_keys_ignored(void)
{
    const char *s =
        "{\"type\":\"target_detection\",\"frame_id\":1,\"extra\":{\"nested\":[1,2,3]},"
        "\"velocity\":[1.0,2.0],\"detections\":[{\"id\":\"x\",\"future_field\":true,"
        "\"bbox\":{\"x\":0,\"y\":0,\"width\":10,\"height\":10,\"z\":99}}]}";
    OrinDetectionMsg m;
    CHECK(parse(s, &m), "unknown keys tolerated");
    CHECK(m.frame_id == 1u, "frame_id past unknown object");
    CHECK(m.num_detections == 1, "detection past unknown keys");
    CHECK(m.detections[0].bbox_w == 10, "bbox after unknown bbox key");
}

static void test_overflow_dropped(void)
{
    char buf[8192];
    int n = snprintf(buf, sizeof(buf), "{\"frame_id\":2,\"detections\":[");
    for (int i = 0; i < ORIN_MAX_DETECTIONS + 5; i++)
    {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"id\":\"%d\",\"confidence\":0.1,\"bbox\":{\"x\":%d,\"y\":0,\"width\":1,\"height\":1}}",
                      i ? "," : "", i, i);
    }
    snprintf(buf + n, sizeof(buf) - n, "]}");

    OrinDetectionMsg m;
    CHECK(parse(buf, &m), "overflow input still parses");
    CHECK(m.num_detections == ORIN_MAX_DETECTIONS, "capped at ORIN_MAX_DETECTIONS");
}

static void test_long_id_truncated(void)
{
    const char *s =
        "{\"frame_id\":3,\"detections\":[{\"id\":\"this_id_is_way_too_long_to_fit\","
        "\"class\":\"HUMAN\",\"confidence\":0.7,\"bbox\":{\"x\":1,\"y\":1,\"width\":1,\"height\":1}}]}";
    OrinDetectionMsg m;
    CHECK(parse(s, &m), "long id parses (truncated)");
    CHECK(strlen(m.detections[0].target_id) == ORIN_ID_MAXLEN - 1, "id truncated to buffer");
    CHECK(m.detections[0].bbox_w == 1, "fields after long id still parse");
}

static void test_malformed_rejected(void)
{
    OrinDetectionMsg m;
    CHECK(!parse("not json", &m), "garbage rejected");
    CHECK(!parse("{\"frame_id\":}", &m), "missing value rejected");
    CHECK(!parse("{\"frame_id\":1", &m), "unterminated object rejected");
    CHECK(!parse("{\"detections\":[{\"id\":\"a\"]}", &m), "broken array rejected");
    CHECK(!orin_detection_msg_parse(NULL, 0, &m), "NULL json rejected");
}

static void test_negative_bbox(void)
{
    /* Letterbox math can produce a slightly negative coord before clamping;
     * the parser must carry the sign through, not choke on '-'. */
    const char *s =
        "{\"frame_id\":9,\"detections\":[{\"id\":\"1\",\"confidence\":0.6,"
        "\"bbox\":{\"x\":-4,\"y\":-2,\"width\":20,\"height\":30}}]}";
    OrinDetectionMsg m;
    CHECK(parse(s, &m), "negative bbox parses");
    CHECK(m.detections[0].bbox_x == -4, "negative x");
    CHECK(m.detections[0].bbox_y == -2, "negative y");
}

int main(void)
{
    test_canonical();
    test_multiple_and_trailing_newline();
    test_missing_frame_id();
    test_unknown_keys_ignored();
    test_overflow_dropped();
    test_long_id_truncated();
    test_malformed_rejected();
    test_negative_bbox();

    if (g_failures == 0)
    {
        printf("PASS: all detection_msg tests\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
}
