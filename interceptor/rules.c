#include "rules.h"
#include "mapper.h"
#include "io_map.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int teaching;    /* Teach Mode in progress */

/* ---------------------------------------------------------------- alerts */
#define ALERT_MAX 128
struct alert { uint64_t t_ns; char code[24]; char text[160]; };
static struct alert alerts[ALERT_MAX];
static unsigned long nalerts;

static void raise_alert(const char *code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void raise_alert(const char *code, const char *fmt, ...)
{
    struct alert *a = &alerts[nalerts++ % ALERT_MAX];
    a->t_ns = ev_since_start_ns();
    snprintf(a->code, sizeof a->code, "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->text, sizeof a->text, fmt, ap);
    va_end(ap);
    ev_alert(code, a->text);
}
unsigned long rules_alert_count(void) { return nalerts; }

/* -------------------------------------------------------- timing (F1-type) */
#define CHAIN_MAX 64
struct chain {
    char out[24]; int out_v; char res[24]; int res_v;
    int have_base, from_teach;
    double base_mean, base_sd;
    double learn[RULE_BASELINE_N]; int nlearn;
    double recent[RULE_TREND_N]; int nrecent, irecent;
    int over_run;               /* consecutive slow samples */
    int warn_active, trend_active;
    double last;
};
static struct chain chains[CHAIN_MAX];
static int nchains;

static struct chain *get_chain(const char *out, int out_v, const char *res, int res_v)
{
    for (int i = 0; i < nchains; i++) {
        struct chain *c = &chains[i];
        if (c->out_v == out_v && c->res_v == res_v && !strcmp(c->out, out) && !strcmp(c->res, res)) return c;
    }
    if (nchains >= CHAIN_MAX) return NULL;
    struct chain *c = &chains[nchains++];
    memset(c, 0, sizeof *c);
    snprintf(c->out, sizeof c->out, "%s", out); c->out_v = out_v;
    snprintf(c->res, sizeof c->res, "%s", res); c->res_v = res_v;
    return c;
}

static const char *onoff(int v) { return v ? "ON" : "off"; }

static void timing_sample(const struct map_sample *s)
{
    if (!s->trusted || teaching) return;     /* Teach Mode sets baselines at finish */
    struct chain *c = get_chain(s->out, s->out_v, s->res, s->res_v);
    if (!c) return;
    c->last = s->ms;

    if (!c->have_base) {                      /* learning the baseline */
        c->learn[c->nlearn++] = s->ms;
        if (c->nlearn == RULE_BASELINE_N) {
            double m = 0, v = 0;
            for (int i = 0; i < c->nlearn; i++) m += c->learn[i];
            m /= c->nlearn;
            for (int i = 0; i < c->nlearn; i++) v += (c->learn[i] - m) * (c->learn[i] - m);
            c->base_mean = m;
            c->base_sd = sqrt(v / (c->nlearn - 1));
            c->have_base = 1;
        }
        return;
    }

    /* spread floor: 5 ms or 2% of the motion, so a perfectly steady motion
     * doesn't alarm on 1 ms of jitter */
    double spread = c->base_sd;
    if (spread < 5.0) spread = 5.0;
    if (spread < 0.02 * c->base_mean) spread = 0.02 * c->base_mean;
    double limit = c->base_mean + RULE_WARN_SIGMA * spread;

    c->over_run = s->ms > limit ? c->over_run + 1 : 0;
    if (c->over_run >= RULE_WARN_RUN && !c->warn_active) {
        c->warn_active = 1;
        raise_alert("F1 TIMING",
            "%s %s -> %s %s took %.0f ms; baseline %.0f ms (limit %.0f), slow %d times in a row",
            s->out, onoff(s->out_v), s->res, onoff(s->res_v), s->ms, c->base_mean, limit, c->over_run);
    }
    if (c->over_run == 0 && c->warn_active && s->ms < c->base_mean + spread) c->warn_active = 0;

    c->recent[c->irecent] = s->ms;
    c->irecent = (c->irecent + 1) % RULE_TREND_N;
    if (c->nrecent < RULE_TREND_N) c->nrecent++;
    if (c->nrecent == RULE_TREND_N) {
        double avg = 0;
        for (int i = 0; i < RULE_TREND_N; i++) avg += c->recent[i];
        avg /= RULE_TREND_N;
        double grow = (avg - c->base_mean) / c->base_mean;
        if (grow >= RULE_TREND_PCT && avg - c->base_mean >= 20.0 && !c->trend_active) {
            c->trend_active = 1;
            raise_alert("F1 TREND",
                "%s %s -> %s %s: %d-cycle average %.0f ms is %.0f%% over baseline %.0f ms",
                s->out, onoff(s->out_v), s->res, onoff(s->res_v), RULE_TREND_N, avg, grow * 100, c->base_mean);
        }
        if (grow < RULE_TREND_PCT / 2) c->trend_active = 0;
    }
}

void rules_set_baseline(const char *out, int out_v, const char *res, int res_v,
                        double mean_ms, double sd_ms, unsigned long n)
{
    (void)n;
    struct chain *c = get_chain(out, out_v, res, res_v);
    if (!c) return;
    c->have_base = 1; c->from_teach = 1;
    c->base_mean = mean_ms; c->base_sd = sd_ms;
    c->nrecent = c->irecent = c->over_run = 0;
    c->warn_active = c->trend_active = 0;
}

static void reset_safety(void);
void rules_reset_baselines(void) { nchains = 0; reset_safety(); }
void rules_set_teaching(int on) { teaching = on; }

/* ------------------------------------------------- tap state for pairs */
#define TAPV_MAX 64
static struct { char name[24]; int v; uint64_t t; } tapv[TAPV_MAX];
static int ntapv;

static int tap_get(const char *name, int *v, uint64_t *t)
{
    for (int i = 0; i < ntapv; i++)
        if (!strcmp(tapv[i].name, name)) { *v = tapv[i].v; if (t) *t = tapv[i].t; return 1; }
    return 0;
}
static void tap_set(const char *name, int v, uint64_t t)
{
    for (int i = 0; i < ntapv; i++)
        if (!strcmp(tapv[i].name, name)) { tapv[i].v = v; tapv[i].t = t; return; }
    if (ntapv < TAPV_MAX) { snprintf(tapv[ntapv].name, 24, "%s", name); tapv[ntapv].v = v; tapv[ntapv].t = t; ntapv++; }
}

/* A pair that must agree: track when it started disagreeing. */
struct pairwatch { uint64_t since; int disagree, alerted; };
static struct pairwatch coilw[N_OF(COIL_CONTACT)], dualw[N_OF(DUAL_CHANNEL)];

static void check_pairs(uint64_t now)
{
    for (int i = 0; i < N_OF(COIL_CONTACT); i++) {
        int a, b; uint64_t ta, tb;
        if (!tap_get(COIL_CONTACT[i][0], &a, &ta) || !tap_get(COIL_CONTACT[i][1], &b, &tb)) continue;
        struct pairwatch *w = &coilw[i];
        if (a != b) {
            if (!w->disagree) { w->disagree = 1; w->since = ta > tb ? ta : tb; w->alerted = 0; }
            double ms = (double)(now - w->since) / 1e6;
            if (ms > RULE_PAIR_MS && !w->alerted) {
                w->alerted = 1;
                raise_alert("F2 MISMATCH",
                    a ? "%s is ON but %s stays off for %.0f ms: contact not closing (burnt/open contact)"
                      : "%s is off but %s stays ON for %.0f ms: contact not opening (WELDED contact)",
                    COIL_CONTACT[i][0], COIL_CONTACT[i][1], ms);
            }
        } else w->disagree = 0;
    }
    for (int i = 0; i < N_OF(DUAL_CHANNEL); i++) {
        int a, b; uint64_t ta, tb;
        if (!tap_get(DUAL_CHANNEL[i][0], &a, &ta) || !tap_get(DUAL_CHANNEL[i][1], &b, &tb)) continue;
        struct pairwatch *w = &dualw[i];
        if (a != b) {
            if (!w->disagree) { w->disagree = 1; w->since = ta < tb ? tb : ta; w->alerted = 0; }
            double ms = (double)(now - w->since) / 1e6;
            if (ms > RULE_PAIR_MS && !w->alerted) {
                w->alerted = 1;
                raise_alert("F3 DISCREPANCY",
                    "safety channels %s (%s) and %s (%s) disagree for %.0f ms; a lazy channel hides behind the good one",
                    DUAL_CHANNEL[i][0], onoff(a), DUAL_CHANNEL[i][1], onoff(b), ms);
            }
        } else {
            if (w->disagree) {
                double ms = (double)(now - w->since) / 1e6;
                if (ms > RULE_PAIR_MS && w->alerted)
                    ev_alert("F3 DISCREPANCY", "channels agree again; total mismatch time logged");
            }
            w->disagree = 0;
        }
    }
}

/* -------------------------------------------- safety response (F4-type) */
/* Each safety device gets its own baseline: an E-stop drops the master
 * control relay first, a guard switch goes straight to the safety relay. */
static int trip_open;              /* waiting for safety contacts to open */
static uint64_t trip_t;
static int trip_src;
static double safety_sum[N_OF(SAFETY_TRIPS)];
static int nsafety[N_OF(SAFETY_TRIPS)];
static double safety_base[N_OF(SAFETY_TRIPS)];   /* 0 = not learned */

void rules_set_safety_baseline_for(const char *trip, double ms)
{
    for (int i = 0; i < N_OF(SAFETY_TRIPS); i++)
        if (!strcmp(SAFETY_TRIPS[i], trip)) { safety_base[i] = ms; safety_sum[i] = 3 * ms; nsafety[i] = 3; }
}
void rules_set_safety_baseline(double ms)
{
    for (int i = 0; i < N_OF(SAFETY_TRIPS); i++) if (!nsafety[i]) rules_set_safety_baseline_for(SAFETY_TRIPS[i], ms);
}
static void reset_safety(void)
{
    memset(safety_sum, 0, sizeof safety_sum); memset(nsafety, 0, sizeof nsafety); memset(safety_base, 0, sizeof safety_base);
}
double rules_finish_teach(void)
{
    double worst = 0;
    for (int i = 0; i < N_OF(SAFETY_TRIPS); i++) {
        if (nsafety[i] > 0 && nsafety[i] < 3) { safety_sum[i] = safety_base[i] * 3; nsafety[i] = 3; }
        if (safety_base[i] > worst) worst = safety_base[i];
    }
    return worst;
}
int rules_safety_baselines(const char **names, double *ms, int max)
{
    int n = 0;
    for (int i = 0; i < N_OF(SAFETY_TRIPS) && n < max; i++)
        if (safety_base[i] > 0) { names[n] = SAFETY_TRIPS[i]; ms[n] = safety_base[i]; n++; }
    return n;
}

static void safety_event(const struct event *e)
{
    /* A trip: a safety input going off while the safety contacts are closed. */
    for (int i = 0; i < N_OF(SAFETY_TRIPS); i++) {
        if (strcmp(e->name, SAFETY_TRIPS[i]) || e->new_v != 0 || e->old_v != 1) continue;
        int k1 = 0, k2 = 0;
        tap_get(SAFETY_CONTACTS[0], &k1, NULL);
        tap_get(SAFETY_CONTACTS[1], &k2, NULL);
        if (!trip_open && (k1 || k2)) { trip_open = 1; trip_t = e->t_ns; trip_src = i; }
    }
    if (!trip_open) return;
    int k1 = 1, k2 = 1;
    tap_get(SAFETY_CONTACTS[0], &k1, NULL);
    tap_get(SAFETY_CONTACTS[1], &k2, NULL);
    if (k1 || k2) return;
    trip_open = 0;                          /* both open: measure */
    int i = trip_src;
    double ms = (double)(e->t_ns - trip_t) / 1e6;
    if (nsafety[i] < 3 || teaching) {       /* learn from the first (or taught) trips */
        safety_sum[i] += ms;
        nsafety[i]++;
        safety_base[i] = safety_sum[i] / nsafety[i];
        char msg[72];
        snprintf(msg, sizeof msg, "%s trip: contacts open in %.0f ms (learning baseline)", SAFETY_TRIPS[i], ms);
        ev_alert("info", msg);
        return;
    }
    double limit = safety_base[i] * RULE_SAFETY_X;
    if (limit < safety_base[i] + 30.0) limit = safety_base[i] + 30.0;
    if (ms > limit)
        raise_alert("F4 SAFETY SLOW",
            "after %s tripped, safety contacts took %.0f ms to open; baseline %.0f ms (limit %.0f)",
            SAFETY_TRIPS[i], ms, safety_base[i], limit);
}

/* Machine allowed to move? (control power + both safety contacts) */
static int power_ok_now(void)
{
    for (int i = 0; i < N_OF(POWER_OK_TAPS); i++) {
        int v;
        if (tap_get(POWER_OK_TAPS[i], &v, NULL) && !v) return 0;
    }
    return 1;
}

/* ------------------------------------------------ broken chain (F8-type) */
static void on_missing(const char *out, int out_v, const char *res, int res_v,
                       double mean_ms, double waited_ms)
{
    if (teaching || !power_ok_now()) return;   /* safety stop: not a chain break */
    raise_alert("F8 BROKEN CHAIN",
        "%s %s, but %s %s never came (normally %.0f ms; waited %.0f ms). Chain broke after the output",
        out, onoff(out_v), res, onoff(res_v), mean_ms, waited_ms);
}

/* --------------------------------------------------------------- public */
void rules_init(void)
{
    nalerts = 0; nchains = 0; ntapv = 0; trip_open = 0; teaching = 0;
    reset_safety();
    memset(coilw, 0, sizeof coilw); memset(dualw, 0, sizeof dualw);
    mapper_on_missing(on_missing);
}

void rules_on_event(const struct event *e)
{
    if (e->src != EV_TAP) return;
    tap_set(e->name, e->new_v, e->t_ns);
    if (e->old_v >= 0) safety_event(e);
    for (int i = 0; i < N_OF(POWER_OK_TAPS); i++)
        if (!strcmp(e->name, POWER_OK_TAPS[i])) { mapper_set_paused(!power_ok_now()); break; }
}

void rules_tick(uint64_t now_ns)
{
    struct map_sample s;
    while (mapper_next_sample(&s)) timing_sample(&s);
    check_pairs(now_ns);
}

size_t rules_format(char *out, size_t max)
{
    size_t len = 0;
#define P(...) do { if (len < max) { int n_ = snprintf(out + len, max - len, __VA_ARGS__); \
                    if (n_ > 0) len += (size_t)n_ < max - len ? (size_t)n_ : max - len - 1; } } while (0)
    P("Interceptor alerts: %lu total (newest last)\n", nalerts);
    unsigned long first = nalerts > 40 ? nalerts - 40 : 0;
    for (unsigned long i = first; i < nalerts; i++) {
        const struct alert *a = &alerts[i % ALERT_MAX];
        P("%6llu.%03llu  %-16s %s\n", (unsigned long long)(a->t_ns / 1000000000ULL),
          (unsigned long long)((a->t_ns % 1000000000ULL) / 1000000), a->code, a->text);
    }
    if (!nalerts) P("  (none)\n");
    P("\nBaselines (motion timing)\n");
    for (int i = 0; i < nchains; i++) {
        const struct chain *c = &chains[i];
        if (!c->have_base) { P("  %-16s %-3s -> %-18s %-3s learning %d/%d\n", c->out, onoff(c->out_v),
                               c->res, onoff(c->res_v), c->nlearn, RULE_BASELINE_N); continue; }
        P("  %-16s %-3s -> %-18s %-3s base %7.1f ms  sd %5.1f  last %7.1f%s%s%s\n", c->out, onoff(c->out_v),
          c->res, onoff(c->res_v), c->base_mean, c->base_sd, c->last, c->from_teach ? "  (teach)" : "",
          c->warn_active ? "  << SLOW" : "", c->trend_active ? "  << TREND" : "");
    }
    P("\nSafety response baselines (trip -> both safety contacts open)\n");
    int any = 0;
    for (int i = 0; i < N_OF(SAFETY_TRIPS); i++)
        if (safety_base[i] > 0) { P("  %-12s %6.0f ms  (%d trips)\n", SAFETY_TRIPS[i], safety_base[i], nsafety[i]); any = 1; }
    if (!any) P("  not measured yet (trip a safety once)\n");
#undef P
    return len;
}
