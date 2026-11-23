#define _POSIX_C_SOURCE 200809L

#include "http_conn.h"
#include "http_parser.h"
#include "http_response.h"
#include "static_file.h"

#include <errno.h>
#include <unistd.h>

static http_conn_result done(int rc)
{
    return rc == 0 ? HTTP_CONN_RESPONDED : HTTP_CONN_ERROR;
}

/* D-30 3·4단계: static_file 결과를 상태 코드로 바꾸고, 200이면 머리 → 본문 → 파일 fd 닫기(D-33). */
static http_conn_result serve_file(int fd, const char *root, const char *target)
{
    static_file file;
    int rc, err;

    switch (static_file_open(root, target, &file)) {
    case STATIC_FILE_OK: break;
    case STATIC_FILE_REDIRECT: return done(http_response_send_status(fd, 301, file.location));
    case STATIC_FILE_BAD_TARGET: return done(http_response_send_status(fd, 400, NULL));
    case STATIC_FILE_FORBIDDEN: return done(http_response_send_status(fd, 403, NULL));
    case STATIC_FILE_NOT_FOUND: return done(http_response_send_status(fd, 404, NULL));
    case STATIC_FILE_ERROR:
    default: return done(http_response_send_status(fd, 500, NULL));
    }
    rc = http_response_send_head(fd, 200, file.content_type, file.size, NULL);
    if (rc == 0) {
        rc = static_file_send_body(fd, &file);
    }
    /* 성공한 close도 errno를 바꿀 수 있으므로 원래 실패 이유를 되돌린다. 다 보낸 뒤의 close 실패는 무시한다. */
    err = errno;
    close(file.fd);
    errno = err;
    return done(rc);
}

http_conn_result http_conn_serve(int fd, const char *root)
{
    char buffer[HTTP_REQUEST_HEAD_MAX];
    http_request request;
    http_parse_result result;
    size_t len = 0;
    int status;

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
    if (result == HTTP_PARSE_OK) {
        return serve_file(fd, root, request.target); /* GET·POST를 같이 처리한다(D-37) */
    }
    switch (result) {
    case HTTP_PARSE_NOT_IMPLEMENTED: status = 501; break;
    case HTTP_PARSE_VERSION_NOT_SUPPORTED: status = 505; break;
    case HTTP_PARSE_BAD_REQUEST:
    default: status = 400; break;
    }
    return done(http_response_send_status(fd, status, NULL)); /* HEAD도 501 + 본문(D-44) */
}
