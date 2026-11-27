#include "taps.h"
#include "events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void taps_init(struct tap_state *t) { memset(t, 0, sizeof *t); }

/* Copy a "quoted" string starting at p (p points at the opening quote). */
static const char *read_str(const char *p, const char *end, char *out, int outlen)
{
    if (p >= end || *p != '"') return NULL;
    p++;
    int k = 0;
    while (p < end && *p != '"') {
        if (k < outlen - 1) out[k++] = *p;
        p++;
    }
    out[k] = 0;
    return p < end ? p + 1 : NULL;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == ':' || *p == ',')) p++;
    return p;
}

static void digital(struct tap_state *t, const char *name, int v)
{
    t->digital_msgs++;
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->name[i], name) == 0) {
            if (t->value[i] != v) { ev_push(EV_TAP, name, t->value[i], v); t->value[i] = v; }
            return;
        }
    }
    if (t->n < TAP_MAX) {
        snprintf(t->name[t->n], sizeof t->name[0], "%s", name);
        t->value[t->n++] = v;
        ev_push(EV_TAP, name, -1, v);
    }
}

static void analog(struct tap_state *t, const char *name, double v)
{
    for (int i = 0; i < t->na; i++)
        if (strcmp(t->aname[i], name) == 0) { t->avalue[i] = v; return; }
    if (t->na < TAP_ANA_MAX) {
        snprintf(t->aname[t->na], sizeof t->aname[0], "%s", name);
        t->avalue[t->na++] = v;
    }
}

static void line(struct tap_state *t, const char *p, const char *end)
{
    const char *k;
    char name[24];
    if ((k = strstr(p, "\"tap\"")) && k < end) {
        const char *q = read_str(skip_ws(k + 5, end), end, name, sizeof name);
        const char *v = q ? strstr(q, "\"v\"") : NULL;
        if (!q || !v || v >= end) { t->bad++; return; }
        digital(t, name, atoi(skip_ws(v + 3, end)));
        return;
    }
    if ((k = strstr(p, "\"a\"")) && k < end) {
        t->analog_frames++;
        const char *q = skip_ws(k + 3, end);
        if (q >= end || *q != '{') { t->bad++; return; }
        q++;
        while (q < end && *q != '}') {
            q = skip_ws(q, end);
            if (q >= end || *q == '}') break;
            q = read_str(q, end, name, sizeof name);
            if (!q) { t->bad++; return; }
            q = skip_ws(q, end);
            char *num_end;
            double v = strtod(q, &num_end);
            if (num_end == q) {            /* true/false/null: treat as 1/0 */
                v = (*q == 't');
                while (q < end && *q != ',' && *q != '}') q++;
            } else q = num_end;
            analog(t, name, v);
        }
        return;
    }
    t->bad++;
}

void taps_feed(struct tap_state *t, const char *buf, int len)
{
    t->packets++;
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', end - p);
        const char *le = nl ? nl : end;
        if (le > p) line(t, p, le);
        p = le + 1;
    }
}
