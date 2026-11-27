/*
 * Teach Mode: the tech walks the machine once; the Interceptor builds its
 * baseline map without ever reading the PLC program.
 *
 *   start               forget old baselines, start learning
 *   label POINT text    give a point a human name ("Clamp closed limit")
 *   finish              freeze baselines from what was learned, save map
 *   cancel              stop teaching, keep the old map
 *
 * Commands arrive by  echo start > /dev/interceptor/teach  (QNX) or on the
 * UDP command port (default 6002) from the teach script. Neither can drive
 * the machine: the Interceptor never writes an output.
 *
 * The taught map is saved to disk and loaded at every start, so the
 * baseline survives a power cycle.
 */
#ifndef TEACH_H
#define TEACH_H
#include <stddef.h>
#include "events.h"

void teach_init(const char *save_path);
/* Load a saved map (baselines + labels). Returns chains loaded, or -1. */
int teach_load(void);
void teach_command(const char *line);
void teach_on_event(const struct event *e);
int teach_active(void);
/* Label for a point, or NULL. */
const char *teach_label(const char *point);
size_t teach_format(char *out, size_t max);
const char *teach_save_path(void);

#endif
