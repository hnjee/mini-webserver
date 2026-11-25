#define _POSIX_C_SOURCE 200809L

/*
 * 연결 하나의 상태 전이(D-57). 루프가 준비 알림·내부 실행마다 http_conn_run을 한 번 부른다.
 * 한 차례는 "출력이 남아 있으면 먼저 보내고(flush), 비면 지금 상태의 처리 함수"를 외부 I/O가 필요할 때까지 반복한다.
 */
#include "http_conn.h"
#include "static_file.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/* 상태(README 상태도). 상태마다 처리 함수 하나가 다음 상태를 정한다. */
enum {
    ST_HEAD,       /* 머리 받기·판정 */
    ST_BODY_START, /* (100을 보낸 뒤) 본문 단계 시작 */
    ST_ECHO,       /* /echo 본문을 받아 되돌림 */
    ST_DISCARD,    /* 버릴 본문 읽기, 다 읽으면 응답 */
    ST_FILE,       /* 파일 조각 채우기 */
    ST_RESPONDED,  /* 응답 출력을 다 보냄: 유지·닫기·정리 판단 */
    ST_DRAIN       /* 닫기 전 정리(D-54) */
};

/* 기다리는 종류(D-59 표의 행). 만료 결과가 이것으로 정해진다. */
enum { WAIT_IDLE, WAIT_READ, WAIT_WRITE, WAIT_DRAIN };

#define CONTINUE (-1) /* 처리 함수가 같은 차례 안에서 계속하라는 값. 그 밖은 http_conn_step */

/* 한 차례의 예산(D-56). */
typedef struct {
    int did_read;     /* 소켓 read 1번 */
    int did_fill;     /* 파일 조각 1번 */
    size_t may_write; /* 차례를 시작할 때 있던 출력 바이트 */
} turn;

typedef int (*state_fn)(http_conn *c, turn *t, long long now);

static int finish(http_conn *c, http_conn_result result, int err)
{
    c->result = result;
    c->err = err;
    return HTTP_CONN_FINISHED;
}

/* 기다림에 들어간다. 마감은 바꾸지 않는다(마감은 진입·진행 사건에서 이미 잡았다, D-59). */
static int wait_for(http_conn *c, int kind)
{
    c->wait = kind;
    return kind == WAIT_WRITE ? HTTP_CONN_WANT_WRITE : HTTP_CONN_WANT_READ;
}

static int would_block(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR; /* EINTR도 이번 차례는 기다림으로 둔다 */
}

/* 출력 버퍼를 n바이트로 채웠다. 쓰기 대기의 마감은 채운 때부터다(D-59). */
static void filled(http_conn *c, size_t n, long long now)
{
    c->out_pos = 0;
    c->out_len = n;
    c->deadline = now + HTTP_CONN_TIMEOUT_NS;
}

/* in 맨 앞 n바이트를 버리고 나머지(다음 요청 시작)를 앞으로 옮긴다(D-47). */
static void consume(http_conn *c, size_t n)
{
    memmove(c->in, c->in + n, c->in_len - n);
    c->in_len -= n;
}

/* 남은 출력을 보낸다. 이번 차례에 채운 출력은 쓰지 않고 RUNNABLE로 다음 회차에 넘긴다(D-56). */
static int flush(http_conn *c, turn *t, long long now)
{
    while (c->out_pos < c->out_len) {
        size_t want = c->out_len - c->out_pos;
        ssize_t n;

        if (t->may_write == 0) {
            return HTTP_CONN_RUNNABLE;
        }
        if (want > t->may_write) {
            want = t->may_write;
        }
        n = write(c->fd, c->out + c->out_pos, want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno == EAGAIN || errno == EWOULDBLOCK ? wait_for(c, WAIT_WRITE) : finish(c, HTTP_CONN_ERROR, errno);
        }
        c->out_pos += (size_t)n;
        t->may_write -= (size_t)n;
        /* 성공한 write는 진행이다. 마지막 write가 곧 "출력을 다 보낸 때"라 다음 대기도 여기서부터 잰다(D-59). */
        c->deadline = now + HTTP_CONN_TIMEOUT_NS;
    }
    c->out_pos = c->out_len = 0;
    return CONTINUE;
}

/* 요청 도중 read가 0이나 -1을 돌려줬을 때(D-53). */
static int mid_request_failure(http_conn *c, ssize_t n)
{
    if (n == 0) {
        return finish(c, HTTP_CONN_PEER_CLOSED, 0);
    }
    return would_block(errno) ? wait_for(c, WAIT_READ) : finish(c, HTTP_CONN_ERROR, errno);
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

/* 상태 응답(머리 + 본문)으로 출력을 채운다. */
static int fill_status(http_conn *c, int status, const http_response_extra *extra, long long now)
{
    int n = http_response_format_status(c->out, sizeof(c->out), status, extra);
    if (n < 0) {
        return finish(c, HTTP_CONN_ERROR, errno);
    }
    filled(c, (size_t)n, now);
    c->state = ST_RESPONDED;
    return CONTINUE;
}

/* 완성된 머리: 유지 여부(D-51)와 본문 위치(D-47)를 정하고 필요하면 100을 채운다(D-50). */
static int start_request(http_conn *c, long long now)
{
    int keep = http_request_keep_alive(&c->req);
    size_t in_buffer = c->in_len - c->req.head_len;

    c->conn_hdr = !keep ? HTTP_CONNECTION_CLOSE
                  : c->req.version_minor == 0 ? HTTP_CONNECTION_KEEP_ALIVE
                                              : HTTP_CONNECTION_NONE;
    c->close_after = keep ? 0 : 1;
    if (in_buffer > c->req.content_length) {
        in_buffer = c->req.content_length;
    }
    c->body_buffered = in_buffer;
    c->body_left = c->req.content_length - in_buffer;
    c->echo = strcmp(c->req.method, "POST") == 0 && is_echo(c->req.target);
    c->state = ST_BODY_START;
    if (wants_continue(&c->req)) {
        int n = http_response_format_continue(c->out, sizeof(c->out));
        if (n < 0) {
            return finish(c, HTTP_CONN_ERROR, errno);
        }
        filled(c, (size_t)n, now);
    }
    return CONTINUE;
}

static int on_head(http_conn *c, turn *t, long long now)
{
    http_parse_result result = c->in_len > 0 ? http_parse_request(c->in, c->in_len, &c->req) : HTTP_PARSE_INCOMPLETE;
    ssize_t n;

    if (result == HTTP_PARSE_OK) {
        return start_request(c, now);
    }
    if (result != HTTP_PARSE_INCOMPLETE) {
        http_response_extra extra = {NULL, NULL, HTTP_CONNECTION_CLOSE};
        c->close_after = 2;
        return fill_status(c, error_status(result), &extra, now);
    }
    /* 파서는 in_len < HTTP_REQUEST_HEAD_MAX에서만 INCOMPLETE를 돌려주므로 남은 칸은 항상 1 이상이다(D-14, D-17). */
    if (t->did_read) {
        return wait_for(c, c->in_len == 0 ? WAIT_IDLE : WAIT_READ);
    }
    t->did_read = 1;
    n = read(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len);
    if (n > 0) {
        c->in_len += (size_t)n;
        c->deadline = now + HTTP_CONN_TIMEOUT_NS;
        return CONTINUE;
    }
    if (c->in_len == 0) { /* 요청 사이(D-53 1행) */
        if (n < 0 && would_block(errno)) {
            return wait_for(c, WAIT_IDLE);
        }
        if (n == 0 || errno == ECONNRESET) {
            return finish(c, HTTP_CONN_DONE, n == 0 ? 0 : errno);
        }
    }
    return mid_request_failure(c, n);
}

/* 본문 단계 시작. /echo만 머리 + 버퍼 속 본문을 먼저 채운다(D-39, D-50). 그 밖은 본문을 버린 뒤 응답한다. */
static int on_body_start(http_conn *c, turn *t, long long now)
{
    size_t used = c->req.head_len + c->body_buffered;

    (void)t;
    if (c->echo) {
        http_response_extra extra = {NULL, NULL, c->conn_hdr};
        int n = http_response_format_head(c->out, sizeof(c->out), 200, "application/octet-stream",
                                          c->req.content_length, &extra);
        if (n < 0) {
            return finish(c, HTTP_CONN_ERROR, errno);
        }
        memcpy(c->out + n, c->in + c->req.head_len, c->body_buffered);
        filled(c, (size_t)n + c->body_buffered, now);
        c->state = ST_ECHO;
    } else {
        c->state = ST_DISCARD;
    }
    consume(c, used); /* 본문 일부만 버퍼에 있었다면 남는 바이트는 없다(D-47) */
    return CONTINUE;
}

/* /echo 본문 조각: 소켓에서 빈 출력 버퍼로 바로 읽어 되돌린다. 머리를 보낸 뒤 실패는 연결만 닫는다(D-51). */
static int on_echo(http_conn *c, turn *t, long long now)
{
    size_t want = c->body_left < sizeof(c->out) ? c->body_left : sizeof(c->out);
    ssize_t n;

    if (c->body_left == 0) {
        c->state = ST_RESPONDED;
        return CONTINUE;
    }
    if (t->did_read) {
        return wait_for(c, WAIT_READ);
    }
    t->did_read = 1;
    n = read(c->fd, c->out, want);
    if (n <= 0) {
        return mid_request_failure(c, n);
    }
    c->body_left -= (size_t)n;
    filled(c, (size_t)n, now); /* 성공한 read이자 출력 채움: 둘 다 now부터 */
    return CONTINUE;
}

/* 판정 결과(D-26 3·4단계)로 최종 응답을 채운다. 200이면 머리를 채우고 파일 조각 단계로 간다. */
static int respond(http_conn *c, long long now)
{
    http_response_extra extra = {NULL, NULL, c->conn_hdr};
    int is_post = strcmp(c->req.method, "POST") == 0, n;
    static_file file;

    if (is_post || is_echo(c->req.target)) {
        extra.allow = is_post ? "GET" : "POST"; /* D-36 */
        return fill_status(c, 405, &extra, now);
    }
    switch (static_file_open(c->root, c->req.target, &file)) {
    case STATIC_FILE_OK: break;
    case STATIC_FILE_REDIRECT:
        extra.location = file.location;
        return fill_status(c, 301, &extra, now);
    case STATIC_FILE_BAD_TARGET:
        extra.connection = HTTP_CONNECTION_CLOSE; /* D-51: 정적 판정 400도 닫고 정리한다 */
        c->close_after = 2;
        return fill_status(c, 400, &extra, now);
    case STATIC_FILE_FORBIDDEN: return fill_status(c, 403, &extra, now);
    case STATIC_FILE_NOT_FOUND: return fill_status(c, 404, &extra, now);
    case STATIC_FILE_ERROR:
    default: return fill_status(c, 500, &extra, now);
    }
    c->file_fd = file.fd; /* 여기부터 파일 fd는 연결이 가진다(release가 닫는다) */
    c->file_left = file.size;
    n = http_response_format_head(c->out, sizeof(c->out), 200, file.content_type, file.size, &extra);
    if (n < 0) {
        return finish(c, HTTP_CONN_ERROR, errno);
    }
    filled(c, (size_t)n, now);
    c->state = ST_FILE;
    return CONTINUE;
}

/* 버릴 본문을 남은 길이만큼만 읽는다(D-39, D-47). 이때 in은 비어 있어 읽기 자리로 쓴다. */
static int on_discard(http_conn *c, turn *t, long long now)
{
    ssize_t n;

    if (c->body_left == 0) {
        return respond(c, now);
    }
    if (t->did_read) {
        return wait_for(c, WAIT_READ);
    }
    t->did_read = 1;
    n = read(c->fd, c->in, c->body_left < sizeof(c->in) ? c->body_left : sizeof(c->in));
    if (n <= 0) {
        return mid_request_failure(c, n);
    }
    c->body_left -= (size_t)n;
    c->deadline = now + HTTP_CONN_TIMEOUT_NS;
    return CONTINUE;
}

/* 파일 조각 하나를 빈 출력 버퍼에 채운다. 원래 크기를 넘게 보내지 않고, 일찍 끝나면 EIO(D-29). */
static int on_file(http_conn *c, turn *t, long long now)
{
    ssize_t n;

    if (c->file_left == 0) {
        close(c->file_fd); /* 다 보낸 뒤의 close 실패는 무시한다 */
        c->file_fd = -1;
        c->state = ST_RESPONDED;
        return CONTINUE;
    }
    if (t->did_fill) {
        return HTTP_CONN_RUNNABLE;
    }
    t->did_fill = 1;
    n = static_file_read_chunk(c->file_fd, c->out, c->file_left < sizeof(c->out) ? c->file_left : sizeof(c->out));
    if (n < 0) {
        return finish(c, HTTP_CONN_ERROR, errno);
    }
    c->file_left -= (size_t)n;
    filled(c, (size_t)n, now);
    return CONTINUE;
}

/* 응답을 다 보냈다: 유지면 다음 요청, 닫기면 끝, 닫는 코드면 shutdown 한 번 뒤 정리(D-51, D-54). */
static int on_responded(http_conn *c, turn *t, long long now)
{
    (void)t;
    if (c->close_after == 1) {
        return finish(c, HTTP_CONN_DONE, 0); /* 본문을 다 읽었으므로 정리 없이 닫는다 */
    }
    if (c->close_after == 2) {
        if (shutdown(c->fd, SHUT_WR) != 0) {
            return finish(c, HTTP_CONN_DONE, 0);
        }
        c->deadline = now + HTTP_CONN_TIMEOUT_NS; /* 정리 마감은 여기서 고정한다(D-59) */
        c->state = ST_DRAIN;
        return CONTINUE;
    }
    c->state = ST_HEAD;
    return CONTINUE;
}

/* 닫기 전 정리: 상대가 닫음·읽기 오류·고정 마감 중 먼저 오는 때까지 읽어 버린다. 마감 뒤에는 새로 읽지 않는다. */
static int on_drain(http_conn *c, turn *t, long long now)
{
    ssize_t n;

    if (now >= c->deadline) {
        return finish(c, HTTP_CONN_DONE, 0);
    }
    if (t->did_read) {
        return wait_for(c, WAIT_DRAIN);
    }
    t->did_read = 1;
    n = read(c->fd, c->in, sizeof(c->in));
    if (n > 0) {
        return CONTINUE; /* 정리 중 읽기는 마감을 늘리지 않는다 */
    }
    if (n < 0 && would_block(errno)) {
        return wait_for(c, WAIT_DRAIN);
    }
    return finish(c, HTTP_CONN_DONE, 0); /* 상대가 닫음, ECONNRESET 등 모든 읽기 오류 */
}

static const state_fn handlers[] = {on_head, on_body_start, on_echo, on_discard, on_file, on_responded, on_drain};

void http_conn_init(http_conn *c, int fd, const char *root, long long now)
{
    c->fd = fd;
    c->root = root;
    c->state = ST_HEAD;
    c->wait = WAIT_IDLE;
    c->deadline = now + HTTP_CONN_TIMEOUT_NS;
    c->result = HTTP_CONN_DONE;
    c->err = 0;
    c->in_len = 0;
    c->close_after = 0;
    c->body_buffered = c->body_left = 0;
    c->file_fd = -1;
    c->file_left = 0;
    c->out_pos = c->out_len = 0;
}

http_conn_step http_conn_run(http_conn *c, long long now)
{
    turn t = {0, 0, c->out_len - c->out_pos};

    for (;;) {
        int r = c->out_pos < c->out_len ? flush(c, &t, now) : handlers[c->state](c, &t, now);
        if (r != CONTINUE) {
            return (http_conn_step)r;
        }
    }
}

int http_conn_expired(const http_conn *c, long long now)
{
    return now >= c->deadline;
}

void http_conn_expire(http_conn *c)
{
    switch (c->wait) {
    case WAIT_READ: finish(c, HTTP_CONN_TIMED_OUT, EAGAIN); break;
    case WAIT_WRITE: finish(c, HTTP_CONN_ERROR, EAGAIN); break;
    default: finish(c, HTTP_CONN_DONE, 0); break; /* 요청 사이·정리 */
    }
}

void http_conn_release(http_conn *c)
{
    if (c->file_fd >= 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
}
