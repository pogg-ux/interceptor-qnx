#include "analog.h"
#include "points.h"
#include "io_map.h"
#include <math.h>
#include <string.h>

void rules_raise(const char *code, const char *text);   /* rules.c */

enum dir { BAD_LOW, BAD_HIGH, BAD_BOTH };

struct metric {
    const char *name, *code, *unit, *meaning;
    enum dir dir;
    double warn_pct;            /* band floor, fraction of baseline */
    double trend_pct;
    /* baseline */
    int have_base, from_teach;
    double base_mean, base_sd;
    double sum, sum2; int n;    /* learning */
    /* recent */
    double recent[10]; int nrecent, irecent;
    int over_run, warn_active, trend_active;
    double last; unsigned long samples;
};

static struct metric M[] = {
    { "press_peak_psi", "F5 PUMP",    "psi", "peak press pressure",           BAD_LOW,  0.08, 0.10 },
    { "press_rise_ms",  "F5 PUMP",    "ms",  "time to 90% of normal pressure", BAD_HIGH, 0.20, 0.25 },
    { "pump_amps",      "F5 PUMP",    "A",   "pump current during stroke",    BAD_HIGH, 0.10, 0.12 },
    { "vibration",      "F5 PUMP",    "mm/s","pump vibration during stroke",  BAD_HIGH, 0.25, 0.30 },
    { "coil_ma",        "F10 COIL",   "mA",  "valve coil current on stroke",  BAD_LOW,  0.10, 0.12 },
    { "stroke_oil_l",   "F16 SEAL",   "L",   "oil used per press stroke",     BAD_HIGH, 0.15, 0.20 },
    { "index_counts",   "F6 ENCODER", "cts", "encoder counts per index",      BAD_BOTH, 0.008, 0.008 },
    { "heater_duty",    "F7 HEATER",  "",    "heater duty to hold temp",      BAD_HIGH, 0.25, 0.25 },
    { "filter_dp_psi",  "F18 FILTER", "psi", "filter pressure drop",          BAD_HIGH, 0.25, 0.25 },
};
#define NM ((int)(sizeof M / sizeof M[0]))
enum { PEAK, RISE, AMPS, VIB, COIL, OIL, IDX, DUTY, FILT };

#define BASE_N 10
static int teaching;

static void sample(int k, double v)
{
    struct metric *m = &M[k];
    m->last = v;
    m->samples++;
    if (teaching || !m->have_base) {
        m->sum += v; m->sum2 += v * v; m->n++;
        if (!teaching && m->n >= BASE_N) {
            m->base_mean = m->sum / m->n;
            double var = (m->sum2 - m->sum * m->sum / m->n) / (m->n - 1);
            m->base_sd = var > 0 ? sqrt(var) : 0;
            m->have_base = 1;
        }
        return;
    }
    double band = 3 * m->base_sd;
    double floor = m->warn_pct * fabs(m->base_mean);
    if (band < floor) band = floor;
    double dev = v - m->base_mean;
    int bad = m->dir == BAD_HIGH ? dev > band : m->dir == BAD_LOW ? -dev > band : fabs(dev) > band;
    m->over_run = bad ? m->over_run + 1 : 0;
    char msg[160];
    if (m->over_run >= 3 && !m->warn_active) {
        m->warn_active = 1;
        snprintf(msg, sizeof msg, "%s %.1f %s vs baseline %.1f (band %.1f), 3 in a row: %s",
                 m->name, v, m->unit, m->base_mean, band, m->meaning);
        rules_raise(m->code, msg);
    }
    if (!bad && fabs(dev) < band / 2) m->warn_active = 0;

    m->recent[m->irecent] = v;
    m->irecent = (m->irecent + 1) % 10;
    if (m->nrecent < 10) m->nrecent++;
    if (m->nrecent == 10 && m->base_mean != 0) {
        double avg = 0;
        for (int i = 0; i < 10; i++) avg += m->recent[i];
        avg /= 10;
        double g = (avg - m->base_mean) / fabs(m->base_mean);
        int tbad = m->dir == BAD_HIGH ? g > m->trend_pct : m->dir == BAD_LOW ? -g > m->trend_pct : fabs(g) > m->trend_pct;
        if (tbad && !m->trend_active) {
            m->trend_active = 1;
            snprintf(msg, sizeof msg, "%s trend: 10-sample average %.1f %s is %+.0f%% vs baseline %.1f: %s",
                     m->name, avg, m->unit, g * 100, m->base_mean, m->meaning);
            rules_raise(m->code, msg);
        }
        if (fabs(g) < m->trend_pct / 2) m->trend_active = 0;
    }
}

/* ---------------------------------------------------------------- episodes */
static int hold_on;   static double hold_peak; static uint64_t hold_t0; static double rise_ms; static int rise_done;
static int stroke_on; static double amps_sum, vib_sum, coil_sum; static int amps_n, vib_n, coil_n;
static int coil_on;
static int index_on;  static double enc_start;
static int cycle_on;  static double duty_sum; static int duty_n;
static int stroke_done_read;

void analog_on_event(const struct event *e)
{
    if (e->old_v < 0) return;
    double v;
    const char *n = e->name;
    if (!strcmp(n, "LS_PRESS_DOWN") && e->new_v) {           /* press reached bottom: hold */
        hold_on = 1; hold_peak = 0; hold_t0 = e->t_ns; rise_done = 0; rise_ms = 0;
        if (coil_on && coil_n) sample(COIL, coil_sum / coil_n);
        coil_on = 0;
    }
    if (!strcmp(n, "Y_PRESS_DOWN") && e->new_v) {            /* stroke starts */
        stroke_on = 1; amps_sum = vib_sum = coil_sum = 0; amps_n = vib_n = coil_n = 0; coil_on = 1;
    }
    if (!strcmp(n, "Y_PRESS_DOWN") && !e->new_v) {           /* stroke + hold end */
        if (hold_on && e->t_ns - hold_t0 >= 200000000ULL) {   /* a real hold, not a touch */
            sample(PEAK, hold_peak);
            if (!rise_done) rise_ms = (double)(e->t_ns - hold_t0) / 1e6;   /* never reached */
            sample(RISE, rise_ms);
        }
        if (stroke_on) {
            if (amps_n) sample(AMPS, amps_sum / amps_n);
            if (vib_n) sample(VIB, vib_sum / vib_n);
        }
        hold_on = stroke_on = coil_on = 0;
        stroke_done_read = 1;
    }
    if (!strcmp(n, "Y_PRESS_UP") && !e->new_v && stroke_done_read) {   /* oil per stroke is final now */
        if (point_analog("LAST_STROKE_L", &v)) sample(OIL, v);
        stroke_done_read = 0;
    }
    if (!strcmp(n, "Y_CR2_INDEX") && e->new_v) {
        if (point_analog("ENC_COUNT", &v)) { index_on = 1; enc_start = v; }
    }
    if (!strcmp(n, "PX_PIN_LOCKED") && e->new_v && index_on) {
        if (point_analog("ENC_COUNT", &v)) {
            double d = v - enc_start;
            if (d < 0) d += ENC_COUNTS_PER_REV;              /* counter wraps once per rev */
            if (d > 100) sample(IDX, d);                     /* ignore jogs / no move */
        }
        index_on = 0;
    }
    if (!strcmp(n, "Y_CLAMP_CLOSE") && e->new_v) {           /* new machine cycle */
        if (cycle_on && duty_n) sample(DUTY, duty_sum / duty_n);
        cycle_on = 1; duty_sum = 0; duty_n = 0;
        if (point_analog("FILTER_DP_PSI", &v)) sample(FILT, v);
    }
}

void analog_on_frame(void)
{
    double v;
    if (hold_on && point_analog("HYD_PSI", &v)) {
        if (v > hold_peak) hold_peak = v;
        double target = 0.9 * (M[PEAK].have_base ? M[PEAK].base_mean : 0);
        if (!rise_done && target > 0 && v >= target) {
            rise_done = 1;
            rise_ms = (double)(ev_since_start_ns() - hold_t0) / 1e6;
        }
    }
    if (stroke_on) {
        /* 4-20 mA card: raw 6400 = 0 A, 32000 = 50 A */
        if (point_analog("AI_PUMP_AMPS", &v)) { amps_sum += (v - 6400.0) / 512.0; amps_n++; }
        if (point_analog("VIBRATION_MM_S", &v)) { vib_sum += v; vib_n++; }
    }
    if (coil_on && point_analog("COIL_MA", &v)) { coil_sum += v; coil_n++; }
    if (cycle_on && point_analog("AO_HEAT_DUTY", &v)) { duty_sum += v; duty_n++; }
}

/* ---------------------------------------------------------------- teach */
void analog_init(void)
{
    for (int k = 0; k < NM; k++) {
        struct metric *m = &M[k];
        m->have_base = m->from_teach = 0; m->sum = m->sum2 = 0; m->n = 0;
        m->nrecent = m->irecent = m->over_run = m->warn_active = m->trend_active = 0;
        m->samples = 0; m->last = 0;
    }
    teaching = 0;
}
void analog_reset_baselines(void)
{
    for (int k = 0; k < NM; k++) {
        M[k].have_base = M[k].from_teach = 0; M[k].sum = M[k].sum2 = 0; M[k].n = 0;
        M[k].nrecent = M[k].irecent = M[k].over_run = M[k].warn_active = M[k].trend_active = 0;
    }
}
void analog_set_teaching(int on) { teaching = on; }

int analog_finish_teach(void)
{
    int done = 0;
    for (int k = 0; k < NM; k++) {
        struct metric *m = &M[k];
        if (m->n < 3) continue;          /* not enough seen while teaching: learn later */
        m->base_mean = m->sum / m->n;
        double var = (m->sum2 - m->sum * m->sum / m->n) / (m->n - 1);
        m->base_sd = var > 0 ? sqrt(var) : 0;
        m->have_base = m->from_teach = 1;
        m->nrecent = m->irecent = m->over_run = 0;
        done++;
    }
    teaching = 0;
    return done;
}

void analog_save(FILE *f)
{
    fprintf(f, "  \"metrics\": [");
    int first = 1;
    for (int k = 0; k < NM; k++) {
        if (!M[k].have_base) continue;
        fprintf(f, "%s\n    {\"metric\": \"%s\", \"mean\": %.3f, \"sd\": %.3f}", first ? "" : ",",
                M[k].name, M[k].base_mean, M[k].base_sd);
        first = 0;
    }
    fprintf(f, "\n  ],\n");
}

void analog_load_line(const char *line)
{
    const char *p = strstr(line, "\"metric\": \"");
    if (!p) return;
    char name[32]; double mean, sd;
    if (sscanf(p, "\"metric\": \"%31[^\"]\", \"mean\": %lf, \"sd\": %lf", name, &mean, &sd) != 3) return;
    for (int k = 0; k < NM; k++)
        if (!strcmp(M[k].name, name)) {
            M[k].base_mean = mean; M[k].base_sd = sd; M[k].have_base = M[k].from_teach = 1;
        }
}

size_t analog_format(char *out, size_t max)
{
    size_t len = 0;
#define P(...) do { if (len < max) { int n_ = snprintf(out + len, max - len, __VA_ARGS__); \
                    if (n_ > 0) len += (size_t)n_ < max - len ? (size_t)n_ : max - len - 1; } } while (0)
    P("\nAnalog / count baselines (one value per stroke or cycle)\n");
    for (int k = 0; k < NM; k++) {
        const struct metric *m = &M[k];
        if (m->have_base)
            P("  %-15s base %9.1f  sd %7.2f  last %9.1f %-4s %-10s%s%s%s\n", m->name, m->base_mean, m->base_sd,
              m->last, m->unit, m->code, m->from_teach ? " (teach)" : "",
              m->warn_active ? "  << OUT" : "", m->trend_active ? "  << TREND" : "");
        else
            P("  %-15s learning %d/%d   last %9.1f %-4s %s\n", m->name, m->n, BASE_N, m->last, m->unit, m->code);
    }
#undef P
    return len;
}
