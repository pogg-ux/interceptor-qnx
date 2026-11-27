/*
 * Interceptor v0 - plain TCP passthrough for QNX 8.0.
 *
 * The PLC connects here (listen port, default 5020). For each PLC connection
 * the Interceptor opens its own connection to the field side (the simulator,
 * default 192.168.122.1:5021) and copies bytes both ways, unchanged.
 * It never creates, changes or drops traffic. v0 only counts what passes.
 *
 * Build (in WSL, after: source ~/qnx800/qnxsdp-env.sh):
 *   qcc -Vgcc_ntox86_64 -O2 -Wall interceptor.c -o interceptor -lsocket
 * Run (on QNX):
 *   ./interceptor [listen_port] [field_host] [field_port]
 */
#include <stdio.h>
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

#define MAX_PAIRS 8
#define BUF_SIZE  4096

struct pair {
    int plc;            /* socket to the PLC (Modbus master) */
    int field;          /* socket to the field side (Modbus slave) */
    unsigned long long to_field, to_plc;   /* bytes forwarded */
    unsigned long req, rsp;                /* read() chunks forwarded */
};

static struct pair pairs[MAX_PAIRS];
static volatile sig_atomic_t running = 1;

static void on_signal(int s) { (void)s; running = 0; }

static void stamp(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    printf("%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
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

/* Write all bytes; returns 0 on success, -1 on error. */
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
    stamp();
    printf("link %d closed (%s): PLC->field %llu bytes, field->PLC %llu bytes\n",
           i, why, pairs[i].to_field, pairs[i].to_plc);
    fflush(stdout);
    close(pairs[i].plc);
    close(pairs[i].field);
    pairs[i].plc = pairs[i].field = -1;
}

int main(int argc, char **argv)
{
    int listen_port = argc > 1 ? atoi(argv[1]) : 5020;
    const char *field_host = argc > 2 ? argv[2] : "192.168.122.1";
    int field_port = argc > 3 ? atoi(argv[3]) : 5021;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_PAIRS; i++) pairs[i].plc = pairs[i].field = -1;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(listen_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 4) < 0) {
        perror("listen");
        return 1;
    }

    stamp();
    printf("Interceptor v0 up. PLC side :%d  ->  field side %s:%d (passthrough, read-only)\n",
           listen_port, field_host, field_port);
    fflush(stdout);

    time_t last_report = time(NULL);
    char buf[BUF_SIZE];

    while (running) {
        struct pollfd pf[1 + 2 * MAX_PAIRS];
        int map[1 + 2 * MAX_PAIRS];   /* pair index for each pollfd */
        int n = 0;
        pf[n].fd = ls; pf[n].events = POLLIN; map[n++] = -1;
        for (int i = 0; i < MAX_PAIRS; i++) {
            if (pairs[i].plc < 0) continue;
            pf[n].fd = pairs[i].plc;   pf[n].events = POLLIN; map[n++] = i;
            pf[n].fd = pairs[i].field; pf[n].events = POLLIN; map[n++] = i;
        }

        int r = poll(pf, n, 1000);
        if (r < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        for (int k = 0; k < n && r > 0; k++) {
            if (!pf[k].revents) continue;
            r--;
            if (map[k] < 0) {
                int c = accept(ls, NULL, NULL);
                if (c < 0) continue;
                set_nodelay(c);
                int slot = -1;
                for (int i = 0; i < MAX_PAIRS; i++) if (pairs[i].plc < 0) { slot = i; break; }
                int f = slot >= 0 ? connect_field(field_host, field_port) : -1;
                if (f < 0) {
                    stamp();
                    printf("PLC connected but field side %s:%d is not reachable; dropping\n",
                           field_host, field_port);
                    fflush(stdout);
                    close(c);
                    continue;
                }
                memset(&pairs[slot], 0, sizeof pairs[slot]);
                pairs[slot].plc = c;
                pairs[slot].field = f;
                stamp();
                printf("link %d open: PLC <-> Interceptor <-> field\n", slot);
                fflush(stdout);
                continue;
            }
            int i = map[k];
            if (pairs[i].plc < 0) continue;   /* closed earlier in this loop */
            int from_plc = (pf[k].fd == pairs[i].plc);
            int src = from_plc ? pairs[i].plc : pairs[i].field;
            int dst = from_plc ? pairs[i].field : pairs[i].plc;
            ssize_t got = read(src, buf, sizeof buf);
            if (got <= 0) { close_pair(i, from_plc ? "PLC hung up" : "field hung up"); continue; }
            if (write_all(dst, buf, got) < 0) { close_pair(i, "write failed"); continue; }
            if (from_plc) { pairs[i].to_field += got; pairs[i].req++; }
            else          { pairs[i].to_plc   += got; pairs[i].rsp++; }
        }

        time_t now = time(NULL);
        if (now - last_report >= 5) {
            last_report = now;
            for (int i = 0; i < MAX_PAIRS; i++) {
                if (pairs[i].plc < 0) continue;
                stamp();
                printf("link %d: %lu requests, %lu replies, %llu/%llu bytes\n",
                       i, pairs[i].req, pairs[i].rsp, pairs[i].to_field, pairs[i].to_plc);
            }
            fflush(stdout);
        }
    }

    for (int i = 0; i < MAX_PAIRS; i++) if (pairs[i].plc >= 0) close_pair(i, "shutdown");
    close(ls);
    stamp(); printf("Interceptor v0 stopped\n");
    return 0;
}
