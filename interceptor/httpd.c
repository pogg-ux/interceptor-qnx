#include "httpd.h"
#include "views.h"
#include "prio.h"
#include "web_files.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/socket.h>

static int ls = -1;
static char body[VIEW_MAX];

static void send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n > 0) {
        ssize_t w = send(fd, c, n, 0);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
        c += w; n -= (size_t)w;
    }
}

static void reply(int fd, int code, const char *type, const void *data, size_t len)
{
    char hdr[256];
    int h = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, code == 200 ? "OK" : code == 405 ? "Method Not Allowed" : "Not Found", type, len);
    send_all(fd, hdr, (size_t)h);
    send_all(fd, data, len);
}

static void handle(int fd)
{
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[1024];
    ssize_t n = recv(fd, req, sizeof req - 1, 0);
    if (n <= 0) return;
    req[n] = 0;
    char method[8], path[128];
    if (sscanf(req, "%7s %127s", method, path) != 2) return;
    if (strcmp(method, "GET")) { reply(fd, 405, "text/plain", "read-only\n", 10); return; }

    if (!strncmp(path, "/api/", 5)) {
        for (int v = 0; v < VIEW_N; v++) {
            if (strcmp(path + 5, VIEW_NAMES[v])) continue;
            size_t len = views_get((enum view_id)v, body, sizeof body);
            reply(fd, 200, v == VIEW_MAPJSON ? "application/json" : "text/plain; charset=utf-8", body, len);
            return;
        }
    } else {
        for (size_t i = 0; i < sizeof WEB_FILES / sizeof WEB_FILES[0]; i++) {
            if (strcmp(path, WEB_FILES[i].path)) continue;
            reply(fd, 200, WEB_FILES[i].type, WEB_FILES[i].data, WEB_FILES[i].len);
            return;
        }
    }
    reply(fd, 404, "text/plain", "not found\n", 10);
}

static void *server(void *arg)
{
    (void)arg;
    prio_set(PRIO_WEB);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; usleep(100000); continue; }
        handle(c);
        close(c);
    }
    return NULL;
}

int httpd_start(int port, char *why, unsigned long why_len)
{
    ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 8) < 0) {
        snprintf(why, why_len, "web dashboard: can't listen on :%d (%s)", port, strerror(errno));
        close(ls);
        return -1;
    }
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, server, NULL) != 0) {
        snprintf(why, why_len, "web dashboard: thread failed");
        return -1;
    }
    pthread_attr_destroy(&at);
    snprintf(why, why_len, "web dashboard on :%d (read-only)", port);
    return 0;
}
