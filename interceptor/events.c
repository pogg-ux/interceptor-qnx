#include "events.h"
#include "hrclock.h"
#include "mapper.h"
#include "rules.h"
#include "teach.h"
#include <string.h>

static struct event ring[EV_RING];
static uint64_t seq;
static uint64_t t0;

void ev_init(void) { t0 = hr_now_ns(); seq = 0; }
uint64_t ev_count(void) { return seq; }
uint64_t ev_since_start_ns(void) { return hr_now_ns() - t0; }

void ev_format(const struct event *e, char *out, size_t len)
{
    static const char *src[] = { "IN ", "OUT", "AO ", "TAP", "-- ", "!! " };
    unsigned long long s = e->t_ns / 1000000000ULL, us = (e->t_ns % 1000000000ULL) / 1000;
    if (e->src == EV_ALERT)
        snprintf(out, len, "%6llu.%06llu  !! ALERT %s: %s", s, us, e->name, e->text);
    else if (e->src == EV_NOTE)
        snprintf(out, len, "%6llu.%06llu  -- %s", s, us, e->name);
    else if (e->old_v < 0)
        snprintf(out, len, "%6llu.%06llu  %s %-18s = %d", s, us, src[e->src], e->name, e->new_v);
    else if (e->src == EV_AO)
        snprintf(out, len, "%6llu.%06llu  %s %-18s %d -> %d", s, us, src[e->src], e->name, e->old_v, e->new_v);
    else
        snprintf(out, len, "%6llu.%06llu  %s %-18s %s", s, us, src[e->src], e->name, e->new_v ? "ON" : "off");
}

void ev_push(enum ev_src src, const char *name, int old_v, int new_v)
{
    struct event *e = &ring[seq % EV_RING];
    e->seq = seq++;
    e->t_ns = hr_now_ns() - t0;
    e->src = src;
    strncpy(e->name, name, sizeof e->name - 1);
    e->name[sizeof e->name - 1] = 0;
    e->old_v = old_v;
    e->new_v = new_v;
    e->text[0] = 0;
    char line[160];
    ev_format(e, line, sizeof line);
    puts(line);
    mapper_on_event(e);
    rules_on_event(e);
    teach_on_event(e);
}

void ev_alert(const char *code, const char *text)
{
    struct event *e = &ring[seq % EV_RING];
    e->seq = seq++;
    e->t_ns = hr_now_ns() - t0;
    e->src = EV_ALERT;
    snprintf(e->name, sizeof e->name, "%s", code);
    snprintf(e->text, sizeof e->text, "%s", text);
    e->old_v = e->new_v = 0;
    char line[160];
    ev_format(e, line, sizeof line);
    puts(line);
}

void ev_dump(FILE *f, int n)
{
    uint64_t have = seq < EV_RING ? seq : EV_RING;
    if ((uint64_t)n > have) n = (int)have;
    char line[160];
    for (uint64_t k = seq - n; k < seq; k++) {
        ev_format(&ring[k % EV_RING], line, sizeof line);
        fprintf(f, "%s\n", line);
    }
}

int ev_recent(struct event *out, int n)
{
    uint64_t have = seq < EV_RING ? seq : EV_RING;
    if ((uint64_t)n > have) n = (int)have;
    for (int i = 0; i < n; i++) out[i] = ring[(seq - n + i) % EV_RING];
    return n;
}
