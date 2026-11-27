/*
 * Usage meters (doc 06): the Interceptor counts the machine's usage itself,
 * from signals it already sees, and keeps the totals on disk. They survive
 * E-stops and power loss, unlike the PLC's own counters.
 */
#ifndef METERS_H
#define METERS_H
#include <stddef.h>
#include <stdint.h>
#include "events.h"

void meters_init(const char *save_path);
void meters_load(void);
void meters_save(void);
void meters_on_event(const struct event *e);
/* Accumulate run-time meters; call often. Saves to disk once a minute. */
void meters_tick(uint64_t now_ns);
size_t meters_format(char *out, size_t max);

#endif
