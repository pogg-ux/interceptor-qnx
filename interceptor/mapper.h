/*
 * Mapper: learns "output -> result -> time" chains with no program access.
 *
 * Every time the PLC switches an output (ON or off), the mapper opens a
 * watch window and records the first change of every input and hardwired
 * tap that follows, with its delay. Over many cycles, results that follow
 * that output edge EVERY time, with a steady delay, become a chain:
 *
 *   OUT Y_CLAMP_CLOSE ON  ->  IN LS_CLAMP_OPEN off   +20 ms
 *                         ->  IN LS_CLAMP_CLOSED ON  +394 ms
 *
 * That is the timing baseline that Teach Mode labels and drift rules watch.
 */
#ifndef MAPPER_H
#define MAPPER_H
#include <stdint.h>
#include "events.h"

#define MAP_WINDOW_NS   4000000000ULL  /* watch up to 4 s after an output edge */
#define MAP_MIN_SEEN    3              /* windows before a chain is trusted */
#define MAP_MIN_RATE    0.8            /* result must follow >= 80% of the time */

void mapper_init(void);
/* Feed every event (called from the event queue). */
void mapper_on_event(const struct event *e);
/* Close windows that have run out of time (call periodically). */
void mapper_tick(uint64_t now_ns);

/* Human-readable chain table. */
size_t mapper_format(char *out, size_t max);
/* Machine-readable map (map.json) for Teach Mode / Confluence. */
size_t mapper_json(char *out, size_t max);

/* Latest finished measurement, for drift rules: returns 1 if a chain
 * (out_edge -> result) has a new sample since the last call. */
struct map_sample {
    char out[24]; int out_v;
    char res[24]; int res_v; enum ev_src res_src;
    double ms, mean_ms, sd_ms;
    unsigned long n;
    int trusted;            /* chain is established (see MAP_MIN_*) */
};
/* Called when an output edge's window ends without a result the chain
 * always had (a broken chain: "output on, cylinder never moved"). */
typedef void (*map_missing_cb)(const char *out, int out_v, const char *res, int res_v,
                               double mean_ms, double waited_ms);
void mapper_on_missing(map_missing_cb cb);
/* Safety stop / control power off: open windows are discarded (a motion
 * cut short by a safety is not a chain failure, and isn't counted). */
void mapper_set_paused(int paused);
int mapper_next_sample(struct map_sample *s);

#endif
