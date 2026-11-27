#include "teach.h"
#include "mapper.h"
#include "rules.h"
#include "analog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LABEL_MAX 96
#define SEEN_MAX  96

static char path[256];
static int active;
static uint64_t t_start;
static char taught_at[32];
static int nchains_taught;
static double safety_taught_ms;

static struct { char point[24]; char label[64]; } labels[LABEL_MAX];
static int nlabels;
static struct { char point[24]; unsigned long changes; } seen[SEEN_MAX];
static int nseen;
static unsigned long trips;

void teach_init(const char *save_path)
{
    snprintf(path, sizeof path, "%s", save_path);
    active = 0; nlabels = 0; nseen = 0; trips = 0;
    taught_at[0] = 0; nchains_taught = 0; safety_taught_ms = 0;
}
const char *teach_save_path(void) { return path; }
int teach_active(void) { return active; }

const char *teach_label(const char *point)
{
    for (int i = 0; i < nlabels; i++) if (!strcmp(labels[i].point, point)) return labels[i].label;
    return NULL;
}

static void set_label(const char *point, const char *text)
{
    for (int i = 0; i < nlabels; i++)
        if (!strcmp(labels[i].point, point)) { snprintf(labels[i].label, sizeof labels[i].label, "%s", text); return; }
    if (nlabels < LABEL_MAX) {
        snprintf(labels[nlabels].point, sizeof labels[0].point, "%s", point);
        snprintf(labels[nlabels].label, sizeof labels[0].label, "%s", text);
        nlabels++;
    }
}

void teach_on_event(const struct event *e)
{
    if (!active || e->old_v < 0) return;
    if (e->src != EV_IN && e->src != EV_TAP && e->src != EV_OUT) return;
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i].point, e->name)) { seen[i].changes++; goto trip; }
    if (nseen < SEEN_MAX) { snprintf(seen[nseen].point, 24, "%s", e->name); seen[nseen++].changes = 1; }
trip:
    if (e->src == EV_TAP && !strcmp(e->name, "T_SR1_K1") && e->new_v == 0) trips++;
}

/* ---------------------------------------------------------------- save/load */
static void save(void)
{
    static char chains[24576];
    size_t n = mapper_json(chains, sizeof chains);
    (void)n;
    FILE *f = fopen(path, "w");
    if (!f) { ev_alert("teach", "could not save the taught map"); return; }
    fprintf(f, "{\n  \"taught_at\": \"%s\",\n  \"safety_response_ms\": %.1f,\n  \"safety\": [", taught_at, safety_taught_ms);
    const char *sn[16]; double sm[16];
    int ns = rules_safety_baselines(sn, sm, 16);
    for (int i = 0; i < ns; i++)
        fprintf(f, "%s\n    {\"trip\": \"%s\", \"response_ms\": %.1f}", i ? "," : "", sn[i], sm[i]);
    fprintf(f, "\n  ],\n");
    analog_save(f);
    fprintf(f, "  \"labels\": [");
    for (int i = 0; i < nlabels; i++)
        fprintf(f, "%s\n    {\"point\": \"%s\", \"label\": \"%s\"}", i ? "," : "", labels[i].point, labels[i].label);
    fprintf(f, "\n  ],\n");
    /* chains block straight from the mapper (one chain per line) */
    const char *c = strstr(chains, "\"chains\"");
    fprintf(f, "  %s", c ? c : "\"chains\": []\n}\n");
    fclose(f);
}

int teach_load(void)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int nch = 0;
    while (fgets(line, sizeof line, f)) {
        char a[24], b[64];
        const char *p;
        if ((p = strstr(line, "\"taught_at\": \""))) sscanf(p + 14, "%31[^\"]", taught_at);
        if ((p = strstr(line, "\"safety_response_ms\": "))) safety_taught_ms = atof(p + 22);
        analog_load_line(line);
        if ((p = strstr(line, "\"trip\": \""))) {
            double ms;
            if (sscanf(p, "\"trip\": \"%23[^\"]\", \"response_ms\": %lf", a, &ms) == 2) rules_set_safety_baseline_for(a, ms);
        }
        if ((p = strstr(line, "\"point\": \"")) && sscanf(p, "\"point\": \"%23[^\"]\", \"label\": \"%63[^\"]\"", a, b) == 2)
            set_label(a, b);
        if ((p = strstr(line, "\"output\": \""))) {
            char out[24], res[24], src[8]; int ov, rv; double mean, sd, mn, mx; unsigned long smp, cyc;
            if (sscanf(p, "\"output\": \"%23[^\"]\", \"output_value\": %d, \"result\": \"%23[^\"]\", \"result_value\": %d, "
                          "\"source\": \"%7[^\"]\", \"mean_ms\": %lf, \"sd_ms\": %lf, \"min_ms\": %lf, \"max_ms\": %lf, "
                          "\"samples\": %lu, \"cycles\": %lu", out, &ov, res, &rv, src, &mean, &sd, &mn, &mx, &smp, &cyc) == 11) {
                rules_set_baseline(out, ov, res, rv, mean, sd, smp);
                nch++;
            }
        }
    }
    fclose(f);

    nchains_taught = nch;
    return nch;
}

/* ---------------------------------------------------------------- commands */
struct finish_ctx { int n; };
static void finish(void)
{
    /* Freeze every established chain's timing as the baseline. */
    static char js[24576];
    mapper_json(js, sizeof js);
    int n = 0;
    for (char *p = strstr(js, "\"output\": \""); p; p = strstr(p + 1, "\"output\": \"")) {
        char out[24], res[24], src[8]; int ov, rv; double mean, sd, mn, mx; unsigned long smp, cyc;
        if (sscanf(p, "\"output\": \"%23[^\"]\", \"output_value\": %d, \"result\": \"%23[^\"]\", \"result_value\": %d, "
                      "\"source\": \"%7[^\"]\", \"mean_ms\": %lf, \"sd_ms\": %lf, \"min_ms\": %lf, \"max_ms\": %lf, "
                      "\"samples\": %lu, \"cycles\": %lu", out, &ov, res, &rv, src, &mean, &sd, &mn, &mx, &smp, &cyc) == 11) {
            rules_set_baseline(out, ov, res, rv, mean, sd, smp);
            n++;
        }
    }
    safety_taught_ms = rules_finish_teach();
    int na = analog_finish_teach();
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(taught_at, sizeof taught_at, "%Y-%m-%dT%H:%M:%SZ", &tm);
    nchains_taught = n;
    active = 0;
    save();
    char msg[72];
    snprintf(msg, sizeof msg, "finished: %d chains, %d analog, safety %.0f ms, %d labels", n, na, safety_taught_ms, nlabels);
    ev_alert("TEACH", msg);
}

void teach_command(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    char cmd[16] = "";
    sscanf(line, "%15s", cmd);
    if (!strcmp(cmd, "start")) {
        mapper_init();
        rules_reset_baselines();
        rules_set_teaching(1);
        analog_reset_baselines();
        analog_set_teaching(1);
        nseen = 0; trips = 0;
        t_start = ev_since_start_ns();
        active = 1;
        ev_alert("TEACH", "started: operate each device, run each motion 5+ times, trip each safety");
    } else if (!strcmp(cmd, "label")) {
        char point[24], text[64] = "";
        if (sscanf(line, "label %23s %63[^\n\r]", point, text) >= 1) set_label(point, text);
    } else if (!strcmp(cmd, "finish")) {
        if (!active) { ev_alert("TEACH", "finish ignored: not teaching"); return; }
        rules_set_teaching(0);
        finish();
    } else if (!strcmp(cmd, "cancel")) {
        active = 0;
        rules_set_teaching(0);
        analog_set_teaching(0);
        teach_load();
        ev_alert("TEACH", "cancelled; previous map kept");
    } else if (cmd[0]) {
        ev_alert("TEACH", "unknown command (use start / label POINT text / finish / cancel)");
    }
}

/* ---------------------------------------------------------------- view */
size_t teach_format(char *out, size_t max)
{
    size_t len = 0;
#define P(...) do { if (len < max) { int n_ = snprintf(out + len, max - len, __VA_ARGS__); \
                    if (n_ > 0) len += (size_t)n_ < max - len ? (size_t)n_ : max - len - 1; } } while (0)
    if (active) {
        unsigned long long s = (ev_since_start_ns() - t_start) / 1000000000ULL;
        P("TEACH MODE: learning (%llus)\n\n", s);
        P("  1. Operate each device / switch     %d points have changed\n", nseen);
        P("  2. Run each motion 5+ times         see /dev/interceptor/map\n");
        P("  3. Trip each safety                 %lu safety trips seen\n", trips);
        P("  4. finish                           echo finish > /dev/interceptor/teach\n\n");
        P("Points seen changing (label them: echo \"label NAME text\" > /dev/interceptor/teach)\n");
        for (int i = 0; i < nseen; i++) {
            const char *l = teach_label(seen[i].point);
            P("  %-20s %5lu changes  %s\n", seen[i].point, seen[i].changes, l ? l : "(no label)");
        }
    } else {
        P("TEACH MODE: off\n");
        if (taught_at[0]) P("  taught at %s: %d motion baselines, slowest safety response %.0f ms, %d labels\n",
                             taught_at, nchains_taught, safety_taught_ms, nlabels);
        else P("  not taught yet: baselines come from the first cycles of normal running\n");
        P("  saved map: %s\n", path);
        P("  re-teach:  echo start > /dev/interceptor/teach\n");
        if (nlabels) {
            P("\nLabels\n");
            for (int i = 0; i < nlabels; i++) P("  %-20s %s\n", labels[i].point, labels[i].label);
        }
    }
#undef P
    return len;
}
