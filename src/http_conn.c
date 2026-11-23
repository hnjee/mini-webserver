#define _POSIX_C_SOURCE 200809L

#include "http_conn.h"
#include "http_parser.h"
#include "http_response.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* 판정 결과별 고정 응답(D-18 본문 제안). 요청 대상과 관계없이 같다. */
static int respond(int fd, http_parse_result result)
{
    static const char *const content_type = "text/plain; charset=utf-8";
    int status;
    const char *body;

    switch (result) {
    case HTTP_PARSE_OK:
        status = 200;
        body = "Hello from mini-webserver\n";
        break;
    case HTTP_PARSE_NOT_IMPLEMENTED:
        status = 501;
        body = "501 Not Implemented\n";
        break;
    case HTTP_PARSE_VERSION_NOT_SUPPORTED:
        status = 505;
        body = "505 HTTP Version Not Supported\n";
        break;
    case HTTP_PARSE_BAD_REQUEST:
    default:
        status = 400;
        body = "400 Bad Request\n";
        break;
    }
    return http_response_send(fd, status, content_type, body, strlen(body));
}

http_conn_result http_conn_serve(int fd)
{
    char buffer[HTTP_REQUEST_HEAD_MAX];
    http_request request;
    http_parse_result result;
    size_t len = 0;

    /* 파서는 len < HTTP_REQUEST_HEAD_MAX에서만 INCOMPLETE를 돌려주므로 남은 칸은 항상 1 이상이다(D-15, D-19). */
    for (;;) {
        ssize_t n = read(fd, buffer + len, sizeof(buffer) - len);
        if (n == 0) {
            return HTTP_CONN_PEER_CLOSED;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return HTTP_CONN_ERROR;
        }
        len += (size_t)n;
        result = http_parse_request(buffer, len, &request);
        if (result != HTTP_PARSE_INCOMPLETE) {
            break;
        }
    }
    return respond(fd, result) == 0 ? HTTP_CONN_RESPONDED : HTTP_CONN_ERROR;
}
