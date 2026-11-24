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
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default: return NULL;
    }
}

/* Connection 머리 줄. 열거형 밖의 값이면 NULL. */
static const char *connection_line(http_connection connection)
{
    switch (connection) {
    case HTTP_CONNECTION_NONE: return "";
    case HTTP_CONNECTION_CLOSE: return "Connection: close\r\n";
    case HTTP_CONNECTION_KEEP_ALIVE: return "Connection: keep-alive\r\n";
    default: return NULL;
    }
}

int http_response_send_head(int fd, int status, const char *content_type,
                            size_t content_length, const http_response_extra *extra)
{
    static const http_response_extra none = {NULL, NULL, HTTP_CONNECTION_NONE};
    const char *reason = reason_phrase(status), *location, *allow, *connection;
    char date[64], head[512 + PATH_MAX]; /* 고정 부분 512바이트 + Location 값 PATH_MAX바이트(D-27) */
    struct tm tm;
    time_t now;
    int n;

    if (extra == NULL) {
        extra = &none;
    }
    location = extra->location;
    allow = extra->allow;
    connection = connection_line(extra->connection);
    if (reason == NULL || connection == NULL || (status == 301) != (location != NULL) ||
        (status == 405) != (allow != NULL)) {
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
                 "%s%s%s"
                 "%s"
                 "\r\n",
                 status, reason, date, content_type, content_length,
                 location ? "Location: " : "", location ? location : "", location ? "\r\n" : "",
                 allow ? "Allow: " : "", allow ? allow : "", allow ? "\r\n" : "", connection);
    if (n < 0 || (size_t)n >= sizeof(head)) {
        errno = EINVAL; /* content_type, location, allow가 지나치게 길다 */
        return -1;
    }
    return io_write_all(fd, head, (size_t)n);
}

int http_response_send(int fd, int status, const char *content_type,
                       const char *body, size_t body_len, const http_response_extra *extra)
{
    if (extra != NULL && (extra->location != NULL || extra->allow != NULL)) {
        errno = EINVAL; /* 301·405는 send_status로 보낸다(D-52) */
        return -1;
    }
    if (http_response_send_head(fd, status, content_type, body_len, extra) != 0) {
        return -1;
    }
    return io_write_all(fd, body, body_len);
}

int http_response_send_status(int fd, int status, const http_response_extra *extra)
{
    const char *reason = reason_phrase(status);
    char body[64];
    int n;

    if (reason == NULL || status == 200) {
        errno = EINVAL;
        return -1;
    }
    /* 본문과 Content-Length가 같은 n에서 나온다(D-33). */
    n = snprintf(body, sizeof(body), "%d %s\n", status, reason);
    if (http_response_send_head(fd, status, "text/plain; charset=utf-8", (size_t)n, extra) != 0) {
        return -1;
    }
    return io_write_all(fd, body, (size_t)n);
}

int http_response_send_continue(int fd)
{
    static const char line[] = "HTTP/1.1 100 Continue\r\n\r\n";
    return io_write_all(fd, line, sizeof(line) - 1);
}
