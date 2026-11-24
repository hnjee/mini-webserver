#define _POSIX_C_SOURCE 200809L

#include "http_conn.h"
#include "http_parser.h"
#include "http_response.h"
#include "io.h"
#include "static_file.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HTTP_CONN_TIMEOUT_USEC 5000000L /* D-41, D-53, D-54: 5초 */

static int set_timeout(int fd, int option, long usec)
{
    struct timeval tv;
    tv.tv_sec = usec / 1000000L;
    tv.tv_usec = usec % 1000000L;
    return setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv));
}

static int timed_out(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

/* read 한 번. EINTR은 다시 읽는다(기다림이 다시 5초가 된다, D-53). */
static ssize_t read_once(int fd, void *buf, size_t cap)
{
    ssize_t n;
    do {
        n = read(fd, buf, cap);
    } while (n < 0 && errno == EINTR);
    return n;
}

/* 요청 도중(머리·본문) read가 0이나 -1을 돌려줬을 때의 결과(D-53). */
static http_conn_result mid_request_failure(ssize_t n)
{
    if (n == 0) {
        return HTTP_CONN_PEER_CLOSED;
    }
    return timed_out() ? HTTP_CONN_TIMED_OUT : HTTP_CONN_ERROR;
}

/*
 * D-54: 쓰기 방향을 닫고, 상대가 닫음·읽기 오류·정리 시작부터 총 5초 중 먼저 오는 때까지 읽어 버린다.
 * read마다 남은 시간으로 SO_RCVTIMEO를 다시 건다. {0,0}은 "제한 없음"이라 남은 시간이 0 이하면 읽지 않는다.
 */
static void drain_before_close(int fd, char *buf, size_t cap)
{
    struct timespec start, now;

    if (shutdown(fd, SHUT_WR) != 0 || clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return;
    }
    for (;;) {
        long long left_ns;
        long left_us;
        ssize_t n;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return;
        }
        left_ns = HTTP_CONN_TIMEOUT_USEC * 1000LL -
                  ((long long)(now.tv_sec - start.tv_sec) * 1000000000LL + (now.tv_nsec - start.tv_nsec));
        if (left_ns <= 0) {
            return;
        }
        left_us = (long)(left_ns / 1000);
        if (left_us == 0) {
            left_us = 1; /* 양수가 {0,0}(제한 없음)으로 내려가지 않게 한다 */
        }
        if (set_timeout(fd, SO_RCVTIMEO, left_us) != 0) {
            return;
        }
        n = read(fd, buf, cap);
        if (n > 0 || (n < 0 && errno == EINTR)) {
            continue;
        }
        return; /* 상대가 닫음, 타임아웃, ECONNRESET 등 모든 읽기 오류 */
    }
}

/*
 * 본문 중 소켓에 남은 remaining 바이트를 buf 크기 조각으로 읽는다(D-39, D-47).
 * echo면 읽은 대로 fd에 돌려주고, 아니면 버린다. 다 읽으면 DONE.
 */
static http_conn_result pump_body(int fd, char *buf, size_t cap, size_t remaining, int echo)
{
    while (remaining > 0) {
        ssize_t n = read_once(fd, buf, remaining < cap ? remaining : cap);
        if (n <= 0) {
            return mid_request_failure(n);
        }
        if (echo && io_write_all(fd, buf, (size_t)n) != 0) {
            return HTTP_CONN_ERROR;
        }
        remaining -= (size_t)n;
    }
    return HTTP_CONN_DONE;
}

/*
 * D-26 3·4단계: static_file 결과를 상태 코드로 바꾸고, 200이면 머리 → 본문 → 파일 fd 닫기(D-29).
 * 반환: 0 보냄, 1 닫는 코드(정적 판정 400)를 보냄, -1 쓰기 실패(errno 보존).
 */
static int serve_file(int fd, const char *root, const char *target, http_connection connection)
{
    http_response_extra extra = {NULL, NULL, connection};
    static_file file;
    int rc, err;

    switch (static_file_open(root, target, &file)) {
    case STATIC_FILE_OK: break;
    case STATIC_FILE_REDIRECT:
        extra.location = file.location;
        return http_response_send_status(fd, 301, &extra);
    case STATIC_FILE_BAD_TARGET:
        extra.connection = HTTP_CONNECTION_CLOSE; /* D-51: 정적 판정 400도 닫는다 */
        return http_response_send_status(fd, 400, &extra) == 0 ? 1 : -1;
    case STATIC_FILE_FORBIDDEN: return http_response_send_status(fd, 403, &extra);
    case STATIC_FILE_NOT_FOUND: return http_response_send_status(fd, 404, &extra);
    case STATIC_FILE_ERROR:
    default: return http_response_send_status(fd, 500, &extra);
    }
    rc = http_response_send_head(fd, 200, file.content_type, file.size, &extra);
    if (rc == 0) {
        rc = static_file_send_body(fd, &file);
    }
    /* 성공한 close도 errno를 바꿀 수 있으므로 원래 실패 이유를 되돌린다. 다 보낸 뒤의 close 실패는 무시한다. */
    err = errno;
    close(file.fd);
    errno = err;
    return rc;
}

/* D-48 순서 1~7의 결과 → 상태 코드. 모두 닫는 코드다(D-37). */
static int error_status(http_parse_result result)
{
    switch (result) {
    case HTTP_PARSE_URI_TOO_LONG: return 414;
    case HTTP_PARSE_HEADER_TOO_LARGE: return 431;
    case HTTP_PARSE_CONTENT_TOO_LARGE: return 413;
    case HTTP_PARSE_NOT_IMPLEMENTED: return 501; /* HEAD도 501 + 본문(D-35) */
    case HTTP_PARSE_VERSION_NOT_SUPPORTED: return 505;
    case HTTP_PARSE_BAD_REQUEST:
    default: return 400;
    }
}

/* D-50: 대상의 첫 '?' 앞이 정확히 /echo인가. */
static int is_echo(const char *target)
{
    return strcspn(target, "?") == 5 && strncmp(target, "/echo", 5) == 0;
}

/* D-50: HTTP/1.1이고 본문이 있고 Expect가 100-continue(대소문자 무시)인가. */
static int wants_continue(const http_request *req)
{
    const char *expect = http_request_header(req, "Expect");
    return req->version_minor == 1 && req->content_length > 0 && expect != NULL &&
           strcasecmp(expect, "100-continue") == 0;
}

http_conn_result http_conn_serve(int fd, const char *root)
{
    char buffer[HTTP_REQUEST_HEAD_MAX]; /* D-47: 맨 앞은 항상 아직 처리하지 않은 요청의 시작이다 */
    http_request request;
    size_t len = 0;

    if (set_timeout(fd, SO_RCVTIMEO, HTTP_CONN_TIMEOUT_USEC) != 0 ||
        set_timeout(fd, SO_SNDTIMEO, HTTP_CONN_TIMEOUT_USEC) != 0) {
        return HTTP_CONN_ERROR;
    }
    for (;;) {
        http_parse_result result = len > 0 ? http_parse_request(buffer, len, &request) : HTTP_PARSE_INCOMPLETE;
        http_response_extra extra = {NULL, NULL, HTTP_CONNECTION_CLOSE};
        size_t in_buffer, remaining;
        http_conn_result r;
        int keep, rc = 0, is_post;

        /* 파서는 len < HTTP_REQUEST_HEAD_MAX에서만 INCOMPLETE를 돌려주므로 남은 칸은 항상 1 이상이다(D-14, D-17). */
        while (result == HTTP_PARSE_INCOMPLETE) {
            ssize_t n = read_once(fd, buffer + len, sizeof(buffer) - len);
            if (n <= 0) {
                if (len == 0 && (n == 0 || timed_out() || errno == ECONNRESET)) {
                    return HTTP_CONN_DONE; /* 요청 사이(D-53 1행) */
                }
                return mid_request_failure(n);
            }
            len += (size_t)n;
            result = http_parse_request(buffer, len, &request);
        }
        if (result != HTTP_PARSE_OK) {
            if (http_response_send_status(fd, error_status(result), &extra) != 0) {
                return HTTP_CONN_ERROR;
            }
            drain_before_close(fd, buffer, sizeof(buffer));
            return HTTP_CONN_DONE;
        }

        /* D-51: 응답 머리를 쓰기 전에 유지 여부를 한 번 정한다. */
        keep = http_request_keep_alive(&request);
        extra.connection = !keep ? HTTP_CONNECTION_CLOSE
                           : request.version_minor == 0 ? HTTP_CONNECTION_KEEP_ALIVE
                                                         : HTTP_CONNECTION_NONE;
        in_buffer = len - request.head_len;
        if (in_buffer > request.content_length) {
            in_buffer = request.content_length;
        }
        remaining = request.content_length - in_buffer;
        if (wants_continue(&request) && http_response_send_continue(fd) != 0) {
            return HTTP_CONN_ERROR;
        }

        is_post = strcmp(request.method, "POST") == 0;
        if (is_post && is_echo(request.target)) {
            /* /echo만 머리를 먼저 보내고 본문을 조각으로 돌려준다(D-39, D-50). 그 뒤 실패는 연결만 닫는다. */
            if (http_response_send_head(fd, 200, "application/octet-stream", request.content_length, &extra) != 0 ||
                io_write_all(fd, buffer + request.head_len, in_buffer) != 0) {
                return HTTP_CONN_ERROR;
            }
            r = pump_body(fd, buffer, sizeof(buffer), remaining, 1);
            if (r != HTTP_CONN_DONE) {
                return r;
            }
        } else {
            /* 나머지는 본문을 다 읽어 버린 뒤 판정·응답한다(D-50). 버퍼를 다시 써도 request는 복사본이라 안전하다. */
            r = pump_body(fd, buffer, sizeof(buffer), remaining, 0);
            if (r != HTTP_CONN_DONE) {
                return r;
            }
            if (is_post || is_echo(request.target)) {
                extra.allow = is_post ? "GET" : "POST"; /* D-36 */
                rc = http_response_send_status(fd, 405, &extra);
            } else {
                rc = serve_file(fd, root, request.target, extra.connection);
            }
            if (rc < 0) {
                return HTTP_CONN_ERROR;
            }
            if (rc == 1) {
                drain_before_close(fd, buffer, sizeof(buffer));
                return HTTP_CONN_DONE;
            }
        }
        if (!keep) {
            return HTTP_CONN_DONE; /* 본문을 다 읽었으므로 정리 없이 닫는다(D-54) */
        }
        /* D-47: 본문 일부만 버퍼에 있었다면 나머지는 소켓에서 읽었으므로 버퍼에 다음 요청 바이트가 없다. */
        if (remaining > 0) {
            len = 0;
        } else {
            size_t used = request.head_len + request.content_length;
            memmove(buffer, buffer + used, len - used);
            len -= used;
        }
    }
}
