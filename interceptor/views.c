#include "views.h"
#include <string.h>

struct views g_views = { .lock = PTHREAD_MUTEX_INITIALIZER };
const char *const VIEW_NAMES[VIEW_N] = { "live", "events", "status" };

void views_set(enum view_id v, const char *text, size_t len)
{
    if (len > VIEW_MAX) len = VIEW_MAX;
    pthread_mutex_lock(&g_views.lock);
    memcpy(g_views.text[v], text, len);
    g_views.len[v] = len;
    pthread_mutex_unlock(&g_views.lock);
}

size_t views_get(enum view_id v, char *out, size_t max)
{
    pthread_mutex_lock(&g_views.lock);
    size_t n = g_views.len[v] < max ? g_views.len[v] : max;
    memcpy(out, g_views.text[v], n);
    pthread_mutex_unlock(&g_views.lock);
    return n;
}
