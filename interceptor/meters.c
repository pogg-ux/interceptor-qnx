#include "meters.h"
#include "points.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Hour meters: seconds a signal has been ON. */
static struct { const char *name, *signal, *pm; double s; } H[] = {
    { "pump_run_hours",     "T_M1_CON",     "hydraulic filter, oil change, pump inspection" },
    { "machine_run_hours",  "LT_RUN",       "general PM" },
    { "control_power_hours","T_CTRL_POWER", "panel fans, electronics" },
    { "heater_on_hours",    "T_SSR_OUT",    "heater element check" },
};
/* Counters: rising edges of a signal. */
static struct { const char *name, *signal; int value; const char *pm; unsigned long n; } C[] = {
    { "cycles",        "LS_EJECT_RET",  1, "tooling inspection" },
    { "clamp_strokes", "Y_CLAMP_CLOSE", 1, "clamp valve / seal inspection" },
    { "press_strokes", "Y_PRESS_DOWN",  1, "press valve / seal inspection" },
    { "index_moves",   "Y_CR2_INDEX",   1, "locating pin and bushing check" },
    { "estops",        "T_ESTOP_A",     0, "(safety history)" },
    { "power_losses",  "T_CTRL_POWER",  0, "(power history)" },
};
#define NH ((int)(sizeof H / sizeof H[0]))
#define NC ((int)(sizeof C / sizeof C[0]))

static char path[256];
static char since[32];
static uint64_t last_ns, last_save_ns;
static int loaded;

void meters_init(const char *save_path)
{
    snprintf(path, sizeof path, "%s", save_path);
    for (int i = 0; i < NH; i++) H[i].s = 0;
    for (int i = 0; i < NC; i++) C[i].n = 0;
    since[0] = 0; last_ns = 0; last_save_ns = 0; loaded = 0;
}

void meters_load(void)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(since, sizeof since, "%Y-%m-%dT%H:%M:%SZ", &tm);
        return;
    }
    char key[48];
    double v;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "since %31s", since) == 1) continue;
        if (sscanf(line, "%47s %lf", key, &v) != 2) continue;
        for (int i = 0; i < NH; i++) if (!strcmp(key, H[i].name)) H[i].s = v * 3600.0;
        for (int i = 0; i < NC; i++) if (!strcmp(key, C[i].name)) C[i].n = (unsigned long)v;
    }
    fclose(f);
    loaded = 1;
}

void meters_save(void)
{
    char tmp[264];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "since %s\n", since);
    for (int i = 0; i < NH; i++) fprintf(f, "%s %.6f\n", H[i].name, H[i].s / 3600.0);
    for (int i = 0; i < NC; i++) fprintf(f, "%s %lu\n", C[i].name, C[i].n);
    fclose(f);
    rename(tmp, path);
}

void meters_on_event(const struct event *e)
{
    if (e->old_v < 0 || (e->src != EV_IN && e->src != EV_OUT && e->src != EV_TAP)) return;
    for (int i = 0; i < NC; i++)
        if (e->new_v == C[i].value && !strcmp(e->name, C[i].signal)) C[i].n++;
}

void meters_tick(uint64_t now)
{
    if (last_ns) {
        double dt = (double)(now - last_ns) / 1e9;
        if (dt > 0 && dt < 5) {
            for (int i = 0; i < NH; i++) {
                int v;
                if (point_digital(H[i].signal, &v) && v) H[i].s += dt;
            }
        }
    }
    last_ns = now;
    if (now - last_save_ns > 60000000000ULL) { last_save_ns = now; meters_save(); }
}

size_t meters_format(char *out, size_t max)
{
    size_t len = 0;
#define P(...) do { if (len < max) { int n_ = snprintf(out + len, max - len, __VA_ARGS__); \
                    if (n_ > 0) len += (size_t)n_ < max - len ? (size_t)n_ : max - len - 1; } } while (0)
    P("Interceptor usage meters (counted by the Interceptor, kept on QNX disk)\n");
    P("continuous since %s%s\n\n", since, loaded ? "" : " (new)");
    for (int i = 0; i < NH; i++)
        P("  %-20s %12.4f h    %-14s PM: %s\n", H[i].name, H[i].s / 3600.0, H[i].signal, H[i].pm);
    P("\n");
    for (int i = 0; i < NC; i++)
        P("  %-20s %12lu      %-14s PM: %s\n", C[i].name, C[i].n, C[i].signal, C[i].pm);
    P("\nsaved to %s once a minute and at shutdown\n", path);
#undef P
    return len;
}
