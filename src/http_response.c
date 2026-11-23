#define _POSIX_C_SOURCE 200809L

#include "http_response.h"
#include "io.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default: return NULL;
    }
}

int http_response_send(int fd, int status, const char *content_type,
                       const char *body, size_t body_len)
{
    const char *reason = reason_phrase(status);
    char date[64], head[512];
    struct tm tm;
    time_t now;
    int n;

    if (reason == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* IMF-fixdate. 서버는 setlocale을 부르지 않으므로 C 로캘의 영어 요일·월 이름이 나온다. */
    now = time(NULL);
    if (now == (time_t)-1 || gmtime_r(&now, &tm) == NULL ||
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0) {
        return -1;
    }
    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Date: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status, reason, date, content_type, body_len);
    if (n < 0 || (size_t)n >= sizeof(head)) {
        errno = EINVAL; /* content_type이 지나치게 길다 */
        return -1;
    }
    if (io_write_all(fd, head, (size_t)n) != 0) {
        return -1;
    }
    return io_write_all(fd, body, body_len);
}
