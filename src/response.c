#include "response.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_HEADER_SIZE 512

const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)   /* 시그널 때문에 끊겼을 뿐이니 다시 쓴다 */
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int send_response(int fd, int status, const char *content_type,
                  const char *body, size_t body_len)
{
    char header[RESPONSE_HEADER_SIZE];
    char date[64];
    time_t now = time(NULL);
    struct tm tm;
    int n;

    /* Date는 항상 GMT로, "Mon, 23 Nov 2026 05:00:00 GMT" 형식 */
    gmtime_r(&now, &tm);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);

    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\n"
                 "Date: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status, status_text(status), date, content_type, body_len);
    if (n < 0 || (size_t)n >= sizeof(header))
        return -1;   /* 헤더가 잘렸다. 잘린 응답은 보내지 않는다 */

    if (write_all(fd, header, (size_t)n) < 0)
        return -1;
    if (body_len > 0 && write_all(fd, body, body_len) < 0)
        return -1;
    return 0;
}

int send_error(int fd, int status)
{
    char body[128];
    int n = snprintf(body, sizeof(body), "%d %s\n", status, status_text(status));

    if (n < 0 || (size_t)n >= sizeof(body))
        return -1;
    return send_response(fd, status, "text/plain", body, (size_t)n);
}
