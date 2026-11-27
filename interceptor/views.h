/*
 * The three views the Interceptor publishes. The main loop rebuilds them
 * 4 times a second; readers (/dev/interceptor/..., /tmp files) get a copy.
 */
#ifndef VIEWS_H
#define VIEWS_H
#include <stddef.h>
#include <pthread.h>

enum view_id { VIEW_LIVE, VIEW_EVENTS, VIEW_STATUS, VIEW_N };

#define VIEW_MAX 32768

struct views {
    pthread_mutex_t lock;
    char   text[VIEW_N][VIEW_MAX];
    size_t len[VIEW_N];
};

extern struct views g_views;
extern const char *const VIEW_NAMES[VIEW_N];   /* "live", "events", "status" */

/* Replace one view (thread-safe). */
void views_set(enum view_id v, const char *text, size_t len);
/* Copy one view out (thread-safe). Returns length copied. */
size_t views_get(enum view_id v, char *out, size_t max);

/* Start the QNX resource manager: /dev/interceptor/{live,events,status}.
 * Returns 0 if running, -1 if not available (not QNX, or not root). */
int resmgr_start(char *why, size_t why_len);

#endif
