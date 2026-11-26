#define _POSIX_C_SOURCE 200809L
#include "access_log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int enabled;

void access_log_enable(void)
{
    enabled = 1; /* 워커를 만들기 전에 한 번만 바꾼다 */
}

void access_log_write(const char *peer, const char *request, int status, size_t bytes,
                      long long started_ns, const char *result)
{
    char line[2400], when[40];
    struct timespec real, mono;
    struct tm local;
    size_t used;
    int n;

    if (!enabled) return;
    clock_gettime(CLOCK_REALTIME, &real);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    localtime_r(&real.tv_sec, &local);
    if (strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S%z", &local) == 0) return;
    n = snprintf(line, sizeof(line), "%s %s \"", when, peer);
    if (n < 0 || (size_t)n >= sizeof(line)) return;
    used = (size_t)n;
    for (; *request && used < sizeof(line) - 64; request++) {
        unsigned char ch = (unsigned char)*request;
        line[used++] = (ch < 0x21 && ch != ' ') || ch > 0x7e || ch == '"' || ch == '\\' ? '?' : (char)ch;
    }
    n = snprintf(line + used, sizeof(line) - used, "\" %d %zu %lldms %s\n", status, bytes,
                 ((long long)mono.tv_sec * 1000000000LL + mono.tv_nsec - started_ns) / 1000000, result);
    if (n < 0 || (size_t)n >= sizeof(line) - used) return;
    (void)fputs(line, stdout); /* stdio의 스트림 잠금으로 워커별 한 줄이 섞이지 않는다 */
}
