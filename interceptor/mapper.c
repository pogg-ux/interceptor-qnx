#include "mapper.h"
#include "io_map.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_OUT_EDGES 64
#define MAX_RESULTS   32
#define SAMPLE_Q      64

struct result {
    char name[24]; int v; enum ev_src src;
    unsigned long seen;          /* windows it appeared in */
    int established;             /* was trusted once: stays trusted */
    double mean, m2, min, max, last;   /* ms */
};

struct out_edge {
    char name[24]; int v;
    unsigned long windows;       /* closed windows */
    struct result r[MAX_RESULTS];
    int nr;
    /* open window */
    int open;
    int interrupted;             /* safety stop during this window */
    uint64_t t0;                 /* when this edge last happened */
    char got[MAX_RESULTS * 2][24];   /* points already recorded this window */
    int ngot;
};

static struct out_edge edges[MAX_OUT_EDGES];
static int nedges;
static struct map_sample sq[SAMPLE_Q];
static unsigned sq_head, sq_tail;

void mapper_init(void) { memset(edges, 0, sizeof edges); nedges = 0; sq_head = sq_tail = 0; }

static struct out_edge *find_edge(const char *name, int v, int create)
{
    for (int i = 0; i < nedges; i++)
        if (edges[i].v == v && strcmp(edges[i].name, name) == 0) return &edges[i];
    if (!create || nedges >= MAX_OUT_EDGES) return NULL;
    struct out_edge *e = &edges[nedges++];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    e->v = v;
    return e;
}

static int trusted(const struct out_edge *oe, const struct result *r)
{
    if (r->established) return 1;
    unsigned long w = oe->windows + (oe->open ? 1 : 0);
    if (r->seen < MAP_MIN_SEEN || w == 0) return 0;
    if ((double)r->seen / (double)w < MAP_MIN_RATE) return 0;
    double sd = r->seen > 1 ? sqrt(r->m2 / (double)(r->seen - 1)) : 0.0;
    if (sd <= 30.0 || sd <= 0.25 * r->mean) {     /* steady delay */
        ((struct result *)r)->established = 1;
        return 1;
    }
    return 0;
}

static map_missing_cb missing_cb;
void mapper_on_missing(map_missing_cb cb) { missing_cb = cb; }

static void add_sample(struct out_edge *oe, const struct event *ev, double ms)
{
    struct result *r = NULL;
    for (int i = 0; i < oe->nr; i++)
        if (oe->r[i].v == ev->new_v && strcmp(oe->r[i].name, ev->name) == 0) { r = &oe->r[i]; break; }
    if (!r) {
        if (oe->nr < MAX_RESULTS) r = &oe->r[oe->nr++];
        else {   /* full: replace the least-seen candidate */
            r = &oe->r[0];
            for (int i = 1; i < oe->nr; i++) if (oe->r[i].seen < r->seen) r = &oe->r[i];
        }
        memset(r, 0, sizeof *r);
        snprintf(r->name, sizeof r->name, "%s", ev->name);
        r->v = ev->new_v;
        r->src = ev->src;
        r->min = r->max = ms;
    }
    r->seen++;
    double d = ms - r->mean;            /* Welford running mean / variance */
    r->mean += d / (double)r->seen;
    r->m2 += d * (ms - r->mean);
    if (ms < r->min) r->min = ms;
    if (ms > r->max) r->max = ms;
    r->last = ms;

    if (sq_head - sq_tail < SAMPLE_Q) {
        struct map_sample *s = &sq[sq_head++ % SAMPLE_Q];
        snprintf(s->out, sizeof s->out, "%s", oe->name); s->out_v = oe->v;
        snprintf(s->res, sizeof s->res, "%s", r->name);  s->res_v = r->v; s->res_src = r->src;
        s->ms = ms; s->mean_ms = r->mean;
        s->sd_ms = r->seen > 1 ? sqrt(r->m2 / (double)(r->seen - 1)) : 0.0;
        s->n = r->seen;
        s->trusted = trusted(oe, r);
    }
}

int mapper_next_sample(struct map_sample *s)
{
    if (sq_tail == sq_head) return 0;
    *s = sq[sq_tail++ % SAMPLE_Q];
    return 1;
}

static int paused;          /* bitmask of PAUSE_* reasons */
void mapper_set_paused(int reason, int on)
{
    int was = paused;
    paused = on ? (paused | reason) : (paused & ~reason);
    if (paused && !was)
        for (int i = 0; i < nedges; i++) if (edges[i].open) edges[i].interrupted = 1;
}

/* Another output edge that is credited with the same result: an output the
 * PLC switches in the same scan, so the cause is ambiguous. Returns:
 *   0 = no twin (this edge alone owns the result)
 *   1 = twin exists and also switched at the same instant this time
 *   2 = twin exists but did NOT switch this time (the result is the twin's)
 *   3 = twin switched too and will report this result (report once per pair) */
static int twin_state(const struct out_edge *oe, const struct result *r)
{
    int have_twin = 0;
    for (int i = 0; i < nedges; i++) {
        const struct out_edge *e = &edges[i];
        if (e == oe) continue;
        for (int k = 0; k < e->nr; k++) {
            const struct result *x = &e->r[k];
            if (x->v != r->v || strcmp(x->name, r->name) || !trusted(e, x)) continue;
            have_twin = 1;
            uint64_t d = e->t0 > oe->t0 ? e->t0 - oe->t0 : oe->t0 - e->t0;
            if (d <= 2000000ULL) return e < oe ? 3 : 1;
        }
    }
    return have_twin ? 2 : 0;
}

static void close_window_at(struct out_edge *oe, uint64_t now_ns, int timed_out)
{
    if (!oe->open) return;
    oe->open = 0;
    if (oe->interrupted) return;           /* cut short by a safety: don't count */
    /* Broken chain: only when the window ran its full time (the output
     * stayed in this state), check every established result happened. */
    if (timed_out && missing_cb) {
        for (int k = 0; k < oe->nr; k++) {
            const struct result *r = &oe->r[k];
            if (!trusted(oe, r)) continue;
            int got = 0;
            for (int j = 0; j < oe->ngot; j++) if (strcmp(oe->got[j], r->name) == 0) { got = 1; break; }
            if (got) continue;
            /* Shared result: only expected if its twin switched too. */
            int tw = twin_state(oe, r);
            if (tw == 2 || tw == 3) continue;
            missing_cb(oe->name, oe->v, r->name, r->v, r->mean, (double)(now_ns - oe->t0) / 1e6);
        }
    }
    oe->windows++;
}
static void close_window(struct out_edge *oe) { close_window_at(oe, 0, 0); }

void mapper_tick(uint64_t now_ns)
{
    for (int i = 0; i < nedges; i++)
        if (edges[i].open && now_ns - edges[i].t0 > MAP_WINDOW_NS) close_window_at(&edges[i], now_ns, 1);
}

void mapper_on_event(const struct event *ev)
{
    if (ev->old_v < 0) return;                     /* first value seen: not an edge */
    if (ev->src == EV_OUT) {
        /* The opposite edge of this output ends its open window. */
        struct out_edge *opp = find_edge(ev->name, !ev->new_v, 0);
        if (opp) close_window(opp);
        struct out_edge *oe = find_edge(ev->name, ev->new_v, 1);
        if (!oe) return;
        close_window(oe);
        oe->open = 1;
        oe->interrupted = paused;
        oe->t0 = ev->t_ns;
        oe->ngot = 0;
        return;
    }
    if (ev->src != EV_IN && ev->src != EV_TAP) return;
    for (int i = 0; i < N_OF(OPERATOR_INPUTS); i++)      /* a person did that */
        if (!strcmp(ev->name, OPERATOR_INPUTS[i])) return;

    /* Credit the result only to the NEAREST cause: the most recent output
     * edge (plus any that switched in the same instant, within 2 ms). */
    uint64_t latest = 0;
    int any = 0;
    for (int i = 0; i < nedges; i++) {
        struct out_edge *oe = &edges[i];
        if (!oe->open) continue;
        if (ev->t_ns - oe->t0 > MAP_WINDOW_NS) { close_window_at(oe, ev->t_ns, 1); continue; }
        if (!any || oe->t0 > latest) latest = oe->t0;
        any = 1;
    }
    if (!any) return;
    for (int i = 0; i < nedges; i++) {
        struct out_edge *oe = &edges[i];
        if (!oe->open || oe->interrupted || oe->t0 + 2000000ULL < latest) continue;
        int dup = 0;                               /* first change of each point only */
        for (int k = 0; k < oe->ngot; k++) if (strcmp(oe->got[k], ev->name) == 0) { dup = 1; break; }
        if (dup) continue;
        if (oe->ngot < MAX_RESULTS * 2) snprintf(oe->got[oe->ngot++], 24, "%s", ev->name);
        add_sample(oe, ev, (double)(ev->t_ns - oe->t0) / 1e6);
    }
}


static int cmp_mean(const void *a, const void *b)
{
    double x = (*(const struct result *const *)a)->mean, y = (*(const struct result *const *)b)->mean;
    return x < y ? -1 : x > y;
}

#include <stdlib.h>
static const char *SRC_TAG[] = { "IN ", "OUT", "AO ", "TAP", "-- " };

size_t mapper_format(char *out, size_t max)
{
    size_t len = 0;
#define P(...) do { if (len < max) { int n_ = snprintf(out + len, max - len, __VA_ARGS__); \
                    if (n_ > 0) len += (size_t)n_ < max - len ? (size_t)n_ : max - len - 1; } } while (0)
    P("Interceptor machine map (learned, no PLC program access)\n");
    P("chain = result follows the output edge >= %.0f%% of the time with a steady delay; min %d cycles\n\n",
      MAP_MIN_RATE * 100, MAP_MIN_SEEN);
    int shown = 0;
    for (int i = 0; i < nedges; i++) {
        struct out_edge *oe = &edges[i];
        const struct result *list[MAX_RESULTS];
        int n = 0;
        for (int k = 0; k < oe->nr; k++) if (trusted(oe, &oe->r[k])) list[n++] = &oe->r[k];
        if (!n) continue;
        qsort(list, n, sizeof list[0], cmp_mean);
        P("OUT %-16s %-3s  (%lu cycles)\n", oe->name, oe->v ? "ON" : "off", oe->windows);
        for (int k = 0; k < n; k++) {
            const struct result *r = list[k];
            double sd = r->seen > 1 ? sqrt(r->m2 / (double)(r->seen - 1)) : 0.0;
            P("   -> %s %-18s %-3s %8.1f ms  sd %5.1f  min %7.1f  max %7.1f  last %7.1f\n",
              SRC_TAG[r->src], r->name, r->v ? "ON" : "off", r->mean, sd, r->min, r->max, r->last);
        }
        shown++;
    }
    if (!shown) P("(learning... run the machine for a few cycles)\n");
#undef P
    return len;
}

size_t mapper_json(char *out, size_t max)
{
    size_t len = 0;
#define P(...) do { if (len < max) { int n_ = snprintf(out + len, max - len, __VA_ARGS__); \
                    if (n_ > 0) len += (size_t)n_ < max - len ? (size_t)n_ : max - len - 1; } } while (0)
    P("{\n  \"version\": 1,\n  \"chains\": [");
    int first = 1;
    for (int i = 0; i < nedges; i++) {
        struct out_edge *oe = &edges[i];
        for (int k = 0; k < oe->nr; k++) {
            const struct result *r = &oe->r[k];
            if (!trusted(oe, r)) continue;
            double sd = r->seen > 1 ? sqrt(r->m2 / (double)(r->seen - 1)) : 0.0;
            P("%s\n    {\"output\": \"%s\", \"output_value\": %d, \"result\": \"%s\", \"result_value\": %d, "
              "\"source\": \"%s\", \"mean_ms\": %.1f, \"sd_ms\": %.1f, \"min_ms\": %.1f, \"max_ms\": %.1f, "
              "\"samples\": %lu, \"cycles\": %lu}",
              first ? "" : ",", oe->name, oe->v, r->name, r->v,
              r->src == EV_TAP ? "tap" : "input", r->mean, sd, r->min, r->max, r->seen, oe->windows);
            first = 0;
        }
    }
    P("\n  ]\n}\n");
#undef P
    return len;
}
