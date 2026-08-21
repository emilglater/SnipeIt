/**
 * detection_msg.c
 *
 * A small recursive-descent JSON scanner specialised to the Orin detection
 * schema (see detection_msg.h). ddl_bridge.c and config.c hand-roll JSON the
 * same way; this one also handles the nested detections array.
 *
 * The scanner is cursor-based and never reads past [json, json+len). Unknown
 * keys are skipped, so the Orin can add fields without breaking the Pi. The
 * input is trusted (our own producer, direct link), but bad input returns
 * false rather than reading garbage.
 */

#include "detection_msg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const char *p;
    const char *end;
} Cur;

static void skip_ws(Cur *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
    {
        c->p++;
    }
}

/* Skip whitespace, then consume `ch` if present. Returns whether it matched. */
static bool eat(Cur *c, char ch)
{
    skip_ws(c);
    if (c->p < c->end && *c->p == ch)
    {
        c->p++;
        return true;
    }
    return false;
}

/* Parse a JSON string token. Copies up to cap-1 bytes into out (out may be
 * NULL to discard), but always scans to the closing quote so the cursor lands
 * correctly even when the value is longer than the destination buffer. */
static bool parse_string(Cur *c, char *out, size_t cap)
{
    skip_ws(c);
    if (c->p >= c->end || *c->p != '"')
    {
        return false;
    }
    c->p++;  /* opening quote */

    size_t i = 0;
    while (c->p < c->end && *c->p != '"')
    {
        char ch = *c->p++;
        if (ch == '\\')
        {
            if (c->p >= c->end)
            {
                return false;
            }
            char esc = *c->p++;
            switch (esc)
            {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'u':
                    /* Skip the 4 hex digits, emit '?'. Our schema is
                     * ASCII-only, so this exists only so a stray \u cannot
                     * desync the cursor. */
                    for (int k = 0; k < 4 && c->p < c->end; k++) c->p++;
                    ch = '?';
                    break;
                default: ch = esc; break;  /* covers \" \\ \/ */
            }
        }
        if (out != NULL && i + 1 < cap)
        {
            out[i++] = ch;
        }
    }

    if (c->p >= c->end || *c->p != '"')
    {
        return false;
    }
    c->p++;  /* closing quote */

    if (out != NULL && cap > 0)
    {
        out[i] = '\0';
    }
    return true;
}

/* Parse a JSON number into a double. */
static bool parse_number(Cur *c, double *out)
{
    skip_ws(c);
    const char *start = c->p;
    if (c->p < c->end && (*c->p == '-' || *c->p == '+'))
    {
        c->p++;
    }
    bool any = false;
    while (c->p < c->end &&
           ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' ||
            *c->p == 'e' || *c->p == 'E' || *c->p == '+' || *c->p == '-'))
    {
        c->p++;
        any = true;
    }
    if (!any)
    {
        return false;
    }

    char buf[64];
    size_t n = (size_t)(c->p - start);
    if (n >= sizeof(buf))
    {
        return false;
    }
    memcpy(buf, start, n);
    buf[n] = '\0';
    *out = strtod(buf, NULL);
    return true;
}

/* Skip exactly one JSON value (object/array/string/number/literal). Used to
 * tolerate unknown keys. Recurses through containers. */
static bool skip_value(Cur *c)
{
    skip_ws(c);
    if (c->p >= c->end)
    {
        return false;
    }

    char ch = *c->p;
    if (ch == '"')
    {
        return parse_string(c, NULL, 0);
    }
    if (ch == '{')
    {
        c->p++;
        if (eat(c, '}'))
        {
            return true;
        }
        do
        {
            if (!parse_string(c, NULL, 0)) return false;  /* key   */
            if (!eat(c, ':'))              return false;
            if (!skip_value(c))            return false;   /* value */
        } while (eat(c, ','));
        return eat(c, '}');
    }
    if (ch == '[')
    {
        c->p++;
        if (eat(c, ']'))
        {
            return true;
        }
        do
        {
            if (!skip_value(c)) return false;
        } while (eat(c, ','));
        return eat(c, ']');
    }

    /* Primitive: number / true / false / null — run to the next delimiter. */
    while (c->p < c->end &&
           *c->p != ',' && *c->p != '}' && *c->p != ']' &&
           *c->p != ' ' && *c->p != '\t' && *c->p != '\n' && *c->p != '\r')
    {
        c->p++;
    }
    return true;
}

static bool parse_bbox(Cur *c, OrinDetection *d)
{
    if (!eat(c, '{'))
    {
        return false;
    }
    if (eat(c, '}'))
    {
        return true;
    }
    do
    {
        char key[12];
        if (!parse_string(c, key, sizeof(key))) return false;
        if (!eat(c, ':'))                       return false;

        double v;
        if (strcmp(key, "x") == 0)
        {
            if (!parse_number(c, &v)) return false;
            d->bbox_x = (int)lround(v);
        }
        else if (strcmp(key, "y") == 0)
        {
            if (!parse_number(c, &v)) return false;
            d->bbox_y = (int)lround(v);
        }
        else if (strcmp(key, "width") == 0)
        {
            if (!parse_number(c, &v)) return false;
            d->bbox_w = (int)lround(v);
        }
        else if (strcmp(key, "height") == 0)
        {
            if (!parse_number(c, &v)) return false;
            d->bbox_h = (int)lround(v);
        }
        else
        {
            if (!skip_value(c)) return false;
        }
    } while (eat(c, ','));

    return eat(c, '}');
}

static bool parse_detection_obj(Cur *c, OrinDetection *d)
{
    if (!eat(c, '{'))
    {
        return false;
    }
    if (eat(c, '}'))
    {
        return true;
    }
    do
    {
        char key[16];
        if (!parse_string(c, key, sizeof(key))) return false;
        if (!eat(c, ':'))                       return false;

        if (strcmp(key, "id") == 0)
        {
            if (!parse_string(c, d->target_id, sizeof(d->target_id))) return false;
        }
        else if (strcmp(key, "class") == 0)
        {
            if (!parse_string(c, d->cls, sizeof(d->cls))) return false;
        }
        else if (strcmp(key, "confidence") == 0)
        {
            double v;
            if (!parse_number(c, &v)) return false;
            d->confidence = (float)v;
        }
        else if (strcmp(key, "bbox") == 0)
        {
            if (!parse_bbox(c, d)) return false;
        }
        else
        {
            if (!skip_value(c)) return false;
        }
    } while (eat(c, ','));

    return eat(c, '}');
}

static bool parse_detections(Cur *c, OrinDetectionMsg *out)
{
    if (!eat(c, '['))
    {
        return false;
    }
    if (eat(c, ']'))
    {
        return true;  /* empty array */
    }
    do
    {
        OrinDetection tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (!parse_detection_obj(c, &tmp)) return false;

        /* Keep parsing past the cap so the cursor stays valid, but drop the
         * overflow detections rather than overrun the fixed array. */
        if (out->num_detections < ORIN_MAX_DETECTIONS)
        {
            out->detections[out->num_detections++] = tmp;
        }
    } while (eat(c, ','));

    return eat(c, ']');
}

bool orin_detection_msg_parse(const char *json, size_t len, OrinDetectionMsg *out)
{
    if (json == NULL || out == NULL)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));

    Cur c = { json, json + len };
    if (!eat(&c, '{'))
    {
        return false;
    }
    if (eat(&c, '}'))
    {
        return true;  /* empty object: valid, just carries nothing */
    }
    do
    {
        char key[16];
        if (!parse_string(&c, key, sizeof(key))) return false;
        if (!eat(&c, ':'))                       return false;

        if (strcmp(key, "frame_id") == 0)
        {
            double v;
            if (!parse_number(&c, &v)) return false;
            out->frame_id     = (uint32_t)v;
            out->has_frame_id = true;
        }
        else if (strcmp(key, "timestamp_ms") == 0)
        {
            double v;
            if (!parse_number(&c, &v)) return false;
            out->timestamp_ms = (uint64_t)v;
        }
        else if (strcmp(key, "detections") == 0)
        {
            if (!parse_detections(&c, out)) return false;
        }
        else
        {
            if (!skip_value(&c)) return false;  /* "type", future fields */
        }
    } while (eat(&c, ','));

    return eat(&c, '}');
}
