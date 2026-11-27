/*
 * Tiny read-only web server for the dashboard (own thread, low priority).
 *   GET /             dashboard page
 *   GET /api/<view>   any /dev/interceptor view as text (live, events, ...)
 * Only GET is accepted; there is no way to change anything through it.
 */
#ifndef HTTPD_H
#define HTTPD_H
int httpd_start(int port, char *why, unsigned long why_len);
#endif
