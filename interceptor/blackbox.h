/*
 * Black box: on every alert, freeze the last 60 s of the event timeline,
 * the live I/O table and the alert to a file on disk, like a flight
 * recorder. Written by a separate low-priority thread so the passthrough
 * never waits on the disk.
 */
#ifndef BLACKBOX_H
#define BLACKBOX_H
#include <stddef.h>

void blackbox_init(const char *dir);
void blackbox_freeze(const char *code, const char *text);
/* For the status view. */
unsigned long blackbox_count(void);
const char *blackbox_last(void);
const char *blackbox_dir(void);

#endif
