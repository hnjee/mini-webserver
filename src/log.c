/*
 * 접근 로그: docs/design/step7.md (S7-06)
 * 응답이 끝날 때(또는 중간에 끊길 때) 한 줄을 write() 한 번으로 남겨요.
 */
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX (HTTP_MAX_URI + 256)

static int g_log_fd = -1;

void access_log_set_fd(int fd)
{
    g_log_fd = fd;
}

static long elapsed_ms(const struct timespec *from)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (long)(now.tv_sec - from->tv_sec) * 1000 + (now.tv_nsec - from->tv_nsec) / 1000000;
    return ms < 0 ? 0 : ms;
}

void access_log_conn(struct conn *c)
{
    if (!c->log_pending)
        return;
    c->log_pending = 0;
    if (g_log_fd >= 0) {
        char when[32];
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);

        char line[LOG_LINE_MAX];
        int n;
        if (c->has_req)
            n = snprintf(line, sizeof(line), "%s %s \"%s %s %s\" %d %zu %ldms\n", when,
                         c->ip[0] ? c->ip : "-", c->req.method, c->req.path, c->req.version,
                         c->status, c->bytes_out, elapsed_ms(&c->t_req));
        else
            n = snprintf(line, sizeof(line), "%s %s \"-\" %d %zu 0ms\n", when,
                         c->ip[0] ? c->ip : "-", c->status, c->bytes_out);
        if (n > 0) {
            size_t len = (size_t)n;
            if (len >= sizeof(line)) {           /* 너무 길면 자르고 줄바꿈으로 끝내요 */
                len = sizeof(line) - 1;
                line[len - 1] = '\n';
            }
            ssize_t w;
            do
                w = write(g_log_fd, line, len);
            while (w < 0 && errno == EINTR);
        }
    }
    c->status = 0;
    c->bytes_out = 0;
    c->has_req = 0;
}
