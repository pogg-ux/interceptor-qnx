/*
 * Interceptor v2 - for QNX 8.0.
 *
 * 1. Passthrough: the PLC connects here (default :5020). The Interceptor
 *    opens its own connection to the field side (default 192.168.122.1:5021)
 *    and copies bytes both ways, unchanged. It never creates, changes or
 *    drops traffic.
 * 2. Copy side: AFTER each chunk is forwarded, a copy goes to the Modbus
 *    parser, which keeps a live table of every PLC input and output.
 * 3. Taps: hardwired signals the PLC can't see (relay coils/contacts, both
 *    safety channels, owner-added sensors) arrive on UDP :6000.
 * 4. Timeline: every change from 2 and 3 goes into ONE event queue, stamped
 *    with ClockCycles() the moment the Interceptor sees it.
 *
 * Outputs (on QNX):
 *   stdout                     every event as it happens
 *   /tmp/interceptor_live      full I/O + tap table, 4x per second
 *   /tmp/interceptor_events    newest 300 events, 4x per second
 *
 * Build (in WSL):  bash deploy.sh
 * Run (on QNX):    ./interceptor [listen_port] [field_host] [field_port] [tap_port]
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "modbus.h"
#include "io_map.h"
#include "events.h"
#include "taps.h"
#include "hrclock.h"

#define MAX_PAIRS 8
#define BUF_SIZE  4096
/* Output files go in /tmp; if /tmp isn't writable, the current folder. */
static char LIVE_PATH[256] = "/tmp/interceptor_live";
static char EVENTS_PATH[256] = "/tmp/interceptor_events";
static char LIVE_TMP[264], EVENTS_TMP[264];

static void pick_output_dir(void)
{
    if (access("/tmp", W_OK) != 0) {
        snprintf(LIVE_PATH, sizeof LIVE_PATH, "./interceptor_live");
        snprintf(EVENTS_PATH, sizeof EVENTS_PATH, "./interceptor_events");
    }
    snprintf(LIVE_TMP, sizeof LIVE_TMP, "%s.tmp", LIVE_PATH);
    snprintf(EVENTS_TMP, sizeof EVENTS_TMP, "%s.tmp", EVENTS_PATH);
}

struct pair {
    int plc;            /* socket to the PLC (Modbus master) */
    int field;          /* socket to the field side (Modbus slave) */
    unsigned long long to_field, to_plc;
};

static struct pair pairs[MAX_PAIRS];
static volatile sig_atomic_t running = 1;
static struct mb_state mb;
static struct tap_state taps;

static void on_signal(int s) { (void)s; running = 0; }

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    unsigned long long t = ev_since_start_ns();
    printf("%6llu.%06llu  -- %s\n", t / 1000000000ULL, (t % 1000000000ULL) / 1000, msg);
}

static const char *point_name(enum mb_table t, int i)
{
    static char tmp[16];
    switch (t) {
    case MB_DI:   if (i < N_OF(DI_NAMES))   return DI_NAMES[i];   break;
    case MB_COIL: if (i < N_OF(COIL_NAMES)) return COIL_NAMES[i]; break;
    case MB_IR:   if (i < N_OF(IR_NAMES))   return IR_NAMES[i];   break;
    case MB_HR:   if (i < N_OF(HR_NAMES))   return HR_NAMES[i];   break;
    default: break;
    }
    snprintf(tmp, sizeof tmp, "?%d", i);
    return tmp;
}

static void on_change(enum mb_table t, int i, int old_v, int new_v)
{
    enum ev_src src = t == MB_DI ? EV_IN : t == MB_COIL ? EV_OUT : EV_AO;
    ev_push(src, point_name(t, i), old_v, new_v);
}

static void write_live(void)
{
    FILE *f = fopen(LIVE_TMP, "w");
    if (!f) return;
    unsigned long long up = ev_since_start_ns() / 1000000000ULL;
    fprintf(f, "Interceptor live   up %llus   events %llu   clock %s\n",
            up, (unsigned long long)ev_count(), hr_source());
    fprintf(f, "Modbus  %lu req / %lu rsp  exc %lu  bad %lu      Taps  %lu pkts  bad %lu\n",
            mb.frames_req, mb.frames_rsp, mb.exceptions, mb.bad_frames, taps.packets, taps.bad);

    static const char *title[] = { "INPUTS (PLC reads)", "OUTPUTS (PLC writes)", "ANALOG IN", "ANALOG OUT" };
    int counts[] = { N_OF(DI_NAMES), N_OF(COIL_NAMES), N_OF(IR_NAMES), N_OF(HR_NAMES) };
    for (int t = 0; t < 4; t++) {
        fprintf(f, "\n%s\n", title[t]);
        for (int i = 0; i < counts[t]; i++) {
            const char *nm = point_name((enum mb_table)t, i);
            if (!mb.known[t][i])                fprintf(f, "  %-20s   -\n", nm);
            else if (t == MB_DI || t == MB_COIL) fprintf(f, "  %-20s %s\n", nm, mb.value[t][i] ? "ON" : " .");
            else                                fprintf(f, "  %-20s %6d\n", nm, mb.value[t][i]);
        }
    }
    fprintf(f, "\nHARDWIRED TAPS (PLC can't see these)\n");
    for (int i = 0; i < taps.n; i++)
        fprintf(f, "  %-20s %s\n", taps.name[i], taps.value[i] ? "ON" : " .");
    fprintf(f, "\nTAPPED ANALOG\n");
    for (int i = 0; i < taps.na; i++)
        fprintf(f, "  %-20s %10.2f\n", taps.aname[i], taps.avalue[i]);
    fclose(f);
    rename(LIVE_TMP, LIVE_PATH);

    f = fopen(EVENTS_TMP, "w");
    if (!f) return;
    fprintf(f, "Interceptor event timeline (newest last)   seconds since start, clock %s\n", hr_source());
    ev_dump(f, 300);
    fclose(f);
    rename(EVENTS_TMP, EVENTS_PATH);
}

static void set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static int connect_field(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    set_nodelay(fd);
    return fd;
}

static int write_all(int fd, const char *buf, ssize_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        buf += w; n -= w;
    }
    return 0;
}

static void close_pair(int i, const char *why)
{
    note("link %d closed (%s): PLC->field %llu bytes, field->PLC %llu bytes",
         i, why, pairs[i].to_field, pairs[i].to_plc);
    close(pairs[i].plc);
    close(pairs[i].field);
    pairs[i].plc = pairs[i].field = -1;
}

static int open_listen(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 4) < 0) return -1;
    return ls;
}

static int open_udp(int port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    return s;
}

int main(int argc, char **argv)
{
    int listen_port = argc > 1 ? atoi(argv[1]) : 5020;
    const char *field_host = argc > 2 ? argv[2] : "192.168.122.1";
    int field_port = argc > 3 ? atoi(argv[3]) : 5021;
    int tap_port = argc > 4 ? atoi(argv[4]) : 6000;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 0; i < MAX_PAIRS; i++) pairs[i].plc = pairs[i].field = -1;
    ev_init();
    pick_output_dir();
    mb_init(&mb, on_change);
    taps_init(&taps);

    int ls = open_listen(listen_port);
    if (ls < 0) { perror("listen"); return 1; }
    int us = open_udp(tap_port);
    if (us < 0) { perror("tap port"); return 1; }

    note("Interceptor v2 up. PLC side :%d -> field %s:%d. Taps on UDP :%d. Clock %s",
         listen_port, field_host, field_port, tap_port, hr_source());
    note("Live table: %s   Timeline: %s", LIVE_PATH, EVENTS_PATH);

    long long last_live_ms = 0;
    char buf[BUF_SIZE + 1];

    while (running) {
        struct pollfd pf[2 + 2 * MAX_PAIRS];
        int map[2 + 2 * MAX_PAIRS];   /* -1 listen, -2 taps, else pair index */
        int n = 0;
        pf[n].fd = ls; pf[n].events = POLLIN; map[n++] = -1;
        pf[n].fd = us; pf[n].events = POLLIN; map[n++] = -2;
        for (int i = 0; i < MAX_PAIRS; i++) {
            if (pairs[i].plc < 0) continue;
            pf[n].fd = pairs[i].plc;   pf[n].events = POLLIN; map[n++] = i;
            pf[n].fd = pairs[i].field; pf[n].events = POLLIN; map[n++] = i;
        }

        int r = poll(pf, n, 250);
        if (r < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        for (int k = 0; k < n && r > 0; k++) {
            if (!pf[k].revents) continue;
            r--;
            if (map[k] == -2) {
                ssize_t got = recv(us, buf, BUF_SIZE, 0);
                if (got > 0) { buf[got] = 0; taps_feed(&taps, buf, (int)got); }
                continue;
            }
            if (map[k] == -1) {
                int c = accept(ls, NULL, NULL);
                if (c < 0) continue;
                set_nodelay(c);
                int slot = -1;
                for (int i = 0; i < MAX_PAIRS; i++) if (pairs[i].plc < 0) { slot = i; break; }
                int f = slot >= 0 ? connect_field(field_host, field_port) : -1;
                if (f < 0) {
                    note("PLC connected but field side %s:%d is not reachable; dropping", field_host, field_port);
                    close(c);
                    continue;
                }
                memset(&pairs[slot], 0, sizeof pairs[slot]);
                pairs[slot].plc = c;
                pairs[slot].field = f;
                mb_reset_streams(&mb);
                note("link %d open: PLC <-> Interceptor <-> field", slot);
                continue;
            }
            int i = map[k];
            if (pairs[i].plc < 0) continue;
            int from_plc = (pf[k].fd == pairs[i].plc);
            int src = from_plc ? pairs[i].plc : pairs[i].field;
            int dst = from_plc ? pairs[i].field : pairs[i].plc;
            ssize_t got = read(src, buf, BUF_SIZE);
            if (got <= 0) { close_pair(i, from_plc ? "PLC hung up" : "field hung up"); continue; }
            if (write_all(dst, buf, got) < 0) { close_pair(i, "write failed"); continue; }
            /* Forwarded. Now read the copy. */
            if (from_plc) { pairs[i].to_field += got; mb_feed_request(&mb, (uint8_t *)buf, got); }
            else          { pairs[i].to_plc   += got; mb_feed_response(&mb, (uint8_t *)buf, got); }
        }

        long long now_ms = (long long)(ev_since_start_ns() / 1000000ULL);
        if (now_ms - last_live_ms >= 250) { last_live_ms = now_ms; write_live(); }
    }

    for (int i = 0; i < MAX_PAIRS; i++) if (pairs[i].plc >= 0) close_pair(i, "shutdown");
    close(ls);
    close(us);
    write_live();
    note("Interceptor stopped");
    return 0;
}
