#define _POSIX_C_SOURCE 200809L

#include "http_response.h"
#include "io.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default: return NULL;
    }
}

int http_response_send_head(int fd, int status, const char *content_type,
                            size_t content_length, const char *location)
{
    const char *reason = reason_phrase(status);
    char date[64], head[512 + PATH_MAX]; /* 고정 부분 512바이트 + Location 값 PATH_MAX바이트(D-31) */
    struct tm tm;
    time_t now;
    int n;

    if (reason == NULL || (status == 301) != (location != NULL)) {
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
                 "%s%s%s"
                 "Connection: close\r\n"
                 "\r\n",
                 status, reason, date, content_type, content_length,
                 location ? "Location: " : "", location ? location : "", location ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof(head)) {
        errno = EINVAL; /* content_type이나 location이 지나치게 길다 */
        return -1;
    }
    return io_write_all(fd, head, (size_t)n);
}

int http_response_send(int fd, int status, const char *content_type,
                       const char *body, size_t body_len)
{
    if (http_response_send_head(fd, status, content_type, body_len, NULL) != 0) {
        return -1;
    }
    return io_write_all(fd, body, body_len);
}

int http_response_send_status(int fd, int status, const char *location)
{
    const char *reason = reason_phrase(status);
    char body[64];
    int n;

    if (reason == NULL || status == 200) {
        errno = EINVAL;
        return -1;
    }
    /* 본문과 Content-Length가 같은 n에서 나온다(D-40). */
    n = snprintf(body, sizeof(body), "%d %s\n", status, reason);
    if (http_response_send_head(fd, status, "text/plain; charset=utf-8", (size_t)n, location) != 0) {
        return -1;
    }
    return io_write_all(fd, body, (size_t)n);
}
