/*
 * Event queue: one ordered timeline of everything the Interceptor sees.
 * Modbus input/output changes and hardwired tap changes go into the same
 * ring buffer, each stamped by hr_now_ns() when the Interceptor saw it.
 */
#ifndef EVENTS_H
#define EVENTS_H
#include <stdint.h>
#include <stdio.h>

enum ev_src { EV_IN, EV_OUT, EV_AO, EV_TAP, EV_NOTE };

struct event {
    uint64_t seq;
    uint64_t t_ns;          /* since Interceptor start */
    enum ev_src src;
    char name[24];
    int old_v, new_v;       /* old_v = -1: first value seen */
};

#define EV_RING 4096

void ev_init(void);
/* Add an event; prints it to stdout too. */
void ev_push(enum ev_src src, const char *name, int old_v, int new_v);
uint64_t ev_count(void);
uint64_t ev_since_start_ns(void);
/* Write the newest n events to f, oldest first. */
void ev_dump(FILE *f, int n);
void ev_format(const struct event *e, char *out, size_t len);
/* Copy the newest n events (oldest first) into out[]; returns how many. */
int ev_recent(struct event *out, int n);

#endif
