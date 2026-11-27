#include "blackbox.h"
#include "events.h"
#include "views.h"
#include "prio.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define BB_SECONDS   60
#define BB_MIN_GAP_S 10           /* at most one freeze every 10 s */
#define BB_KEEP      50

static char dir[200];
static unsigned long count;
static char last[256];
static uint64_t last_t;

struct job { char path[256]; char *text; size_t len; };

static void *writer(void *arg)
{
    struct job *j = arg;
    prio_set(PRIO_BLACKBOX);
    FILE *f = fopen(j->path, "w");
    if (f) { fwrite(j->text, 1, j->len, f); fclose(f); }
    free(j->text);
    free(j);
    return NULL;
}

void blackbox_init(const char *d)
{
    snprintf(dir, sizeof dir, "%s", d);
    mkdir(dir, 0755);
    count = 0; last[0] = 0; last_t = 0;
}
unsigned long blackbox_count(void) { return count; }
const char *blackbox_last(void) { return last[0] ? last : "(none yet)"; }
const char *blackbox_dir(void) { return dir; }

void blackbox_freeze(const char *code, const char *text)
{
    uint64_t now = ev_since_start_ns();
    if (count && now - last_t < (uint64_t)BB_MIN_GAP_S * 1000000000ULL) return;
    last_t = now;

    size_t cap = 512 * 1024;
    char *buf = malloc(cap);
    if (!buf) return;
    size_t len = 0;
#define P(...) do { if (len < cap) { int n_ = snprintf(buf + len, cap - len, __VA_ARGS__); \
                    if (n_ > 0) len += (size_t)n_ < cap - len ? (size_t)n_ : cap - len - 1; } } while (0)
    time_t wall = time(NULL);
    char ts[32];
    struct tm tm;
    gmtime_r(&wall, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    P("INTERCEPTOR BLACK BOX\n=====================\nfrozen   %s (t = %.3f s)\nalert    %s: %s\n\n",
      ts, (double)now / 1e9, code, text);

    P("--- last %d s of the timeline ---\n", BB_SECONDS);
    static struct event ev[EV_RING];
    int n = ev_recent(ev, EV_RING);
    char line[160];
    for (int i = 0; i < n; i++) {
        if (now - ev[i].t_ns > (uint64_t)BB_SECONDS * 1000000000ULL) continue;
        ev_format(&ev[i], line, sizeof line);
        P("%s\n", line);
    }
    P("\n--- I/O and taps at the moment of the alert ---\n");
    static char live[VIEW_MAX];
    size_t ll = views_get(VIEW_LIVE, live, sizeof live - 1);
    live[ll] = 0;
    P("%s\n", live);
#undef P

    struct job *j = malloc(sizeof *j);
    if (!j) { free(buf); return; }
    char safe[24];
    snprintf(safe, sizeof safe, "%s", code);
    for (char *c = safe; *c; c++) if (*c == ' ' || *c == '/') *c = '_';
    snprintf(j->path, sizeof j->path, "%s/bb_%03lu_%s.txt", dir, count % BB_KEEP, safe);
    snprintf(last, sizeof last, "%s", j->path);
    j->text = buf;
    j->len = len;
    count++;

    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, writer, j) != 0) writer(j);
    pthread_attr_destroy(&a);
}
