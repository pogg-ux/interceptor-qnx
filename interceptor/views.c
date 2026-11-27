#include "views.h"
#include <stdio.h>
#include <string.h>

struct views g_views = { .lock = PTHREAD_MUTEX_INITIALIZER };
const char *const VIEW_NAMES[VIEW_N] = { "live", "events", "status", "map", "map.json", "alerts", "teach", "meters" };

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

#define CMDQ 16
static char cmdq[CMDQ][128];
static unsigned cmd_head, cmd_tail;

void views_push_cmd(const char *line)
{
    pthread_mutex_lock(&g_views.lock);
    /* one command per line */
    const char *p = line;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        if (e > p && cmd_head - cmd_tail < CMDQ) {
            size_t n = (size_t)(e - p) < sizeof cmdq[0] - 1 ? (size_t)(e - p) : sizeof cmdq[0] - 1;
            memcpy(cmdq[cmd_head % CMDQ], p, n);
            cmdq[cmd_head % CMDQ][n] = 0;
            cmd_head++;
        }
        p = *e ? e + 1 : e;
    }
    pthread_mutex_unlock(&g_views.lock);
}

int views_pop_cmd(char *out, size_t max)
{
    int got = 0;
    pthread_mutex_lock(&g_views.lock);
    if (cmd_tail != cmd_head) {
        snprintf(out, max, "%s", cmdq[cmd_tail % CMDQ]);
        cmd_tail++;
        got = 1;
    }
    pthread_mutex_unlock(&g_views.lock);
    return got;
}
