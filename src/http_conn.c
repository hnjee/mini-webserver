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
    ST_DRAIN,      /* 닫기 전 정리(D-54) */
    ST_PX_CONNECT, /* 백엔드 연결 완료 대기(EINPROGRESS, D-67) */
    ST_PX_UPLOAD,  /* 요청 머리·본문을 백엔드로 보내며 조기 응답을 지켜봄(D-64, D-66) */
    ST_PX_HEAD,    /* 응답 머리 받기 */
    ST_PX_BODY,    /* 커밋 뒤 본문 중계(D-65) */
    ST_PX_SKIP     /* 남은 요청 본문 버리기 → 저장한 502·504 또는 유지·닫기 판단(D-66) */
};

/* 기다리는 종류(D-59·D-67 표의 행). 만료 결과가 이것으로 정해진다. */
enum { WAIT_IDLE, WAIT_READ, WAIT_WRITE, WAIT_DRAIN, WAIT_BE_CONNECT, WAIT_BE_WRITE, WAIT_BE_READ };

#define CONTINUE (-1) /* 처리 함수가 같은 차례 안에서 계속하라는 값. 그 밖은 http_conn_step */

/* 한 차례의 예산(D-56). */
typedef struct {
    int did_read;     /* 소켓 read 1번 */
    int did_fill;     /* 파일 조각 1번 */
    size_t may_write; /* 차례를 시작할 때 있던 출력 바이트 */
    int did_be_write; /* 백엔드 write 1번(D-67) */
    int wake;         /* 깨운 원인(HTTP_CONN_WAKE_*). 연결 완료 판정에 한 번 쓴다 */
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
static int px_start(http_conn *c, route_rule *rule, long long now);

static int on_body_start(http_conn *c, turn *t, long long now)
{
    size_t used = c->req.head_len + c->body_buffered;
    route_rule *rule = route_match(c->routes, c->req.target);

    (void)t;
    if (rule != NULL) { /* D-62: 규칙 매칭이 /echo·405·정적 판정보다 먼저다 */
        return px_start(c, rule, now);
    }
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

/* ---- Step 6 프록시(D-64~D-67) ---- */

/* 프록시 대기에 들어간다. 대기 종류가 바뀌면 그때부터 새 마감을 잡는다(D-67). 같은 종류면 마감을 늘리지 않는다. */
static int wait_px(http_conn *c, int kind, long long now)
{
    if (c->wait != kind) {
        c->wait = kind;
        c->deadline = now + HTTP_CONN_TIMEOUT_NS;
    }
    switch (kind) {
    case WAIT_BE_CONNECT:
    case WAIT_BE_WRITE: return HTTP_CONN_WANT_BACKEND_WRITE;
    case WAIT_BE_READ: return HTTP_CONN_WANT_BACKEND_READ;
    default: return HTTP_CONN_WANT_READ;
    }
}

/* 백엔드 소켓을 닫는다. close가 epoll 등록도 지운다(dup 없음). 세대가 바뀌어 옛 알림이 걸러진다. */
static void close_backend(http_conn *c)
{
    if (c->be_fd >= 0) {
        close(c->be_fd);
        c->be_fd = -1;
        c->be_gen++;
    }
    c->uploading = 0;
    c->be_out_pos = c->be_out_len = 0; /* 미송신 바이트는 버린다(이미 받은 본문으로 센다, D-66) */
}

/* D-66: 커밋 전 실패. 백엔드를 닫고, 남은 본문을 다 버린 뒤 status를 보낸다. */
static int px_fail(http_conn *c, int status)
{
    close_backend(c);
    c->pending_status = status;
    c->state = ST_PX_SKIP;
    return CONTINUE;
}

static void start_upload(http_conn *c)
{
    c->uploading = 1;
    c->state = ST_PX_UPLOAD;
}

/* 규칙에 맞는 요청: 백엔드를 고르고(D-63) 요청 머리 + 버퍼 속 본문을 백엔드 버퍼에 만든 뒤 논블로킹 connect(D-64). */
static int px_start(http_conn *c, route_rule *rule, long long now)
{
    const route_backend *b = route_pick(rule);
    size_t used = c->req.head_len + c->body_buffered;
    int n = proxy_format_request(c->be_out, sizeof(c->be_out), &c->req, route_forward_target(rule, c->req.target),
                                 b->authority);

    if (n < 0 || (size_t)n + c->body_buffered > sizeof(c->be_out)) {
        return finish(c, HTTP_CONN_ERROR, EINVAL); /* 16KB는 머리 8KB + 덧붙이는 머리보다 크다(D-67): 오지 않는 경로 */
    }
    memcpy(c->be_out + n, c->in + c->req.head_len, c->body_buffered);
    c->be_out_pos = 0;
    c->be_out_len = (size_t)n + c->body_buffered;
    consume(c, used); /* 본문 뒤 다음 요청 바이트는 남긴다(D-47) */
    c->be_in_len = 0;
    c->committed = 0;
    c->pending_status = 0;
    c->be_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->be_fd < 0) {
        return px_fail(c, 502);
    }
    c->be_gen++;
    if (connect(c->be_fd, (const struct sockaddr *)&b->addr, sizeof(b->addr)) == 0) {
        start_upload(c);
        return CONTINUE;
    }
    if (errno != EINPROGRESS) {
        return px_fail(c, 502);
    }
    c->state = ST_PX_CONNECT;
    c->wait = WAIT_BE_CONNECT;
    c->deadline = now + HTTP_CONN_TIMEOUT_NS; /* 연결 대기는 시작부터 5초 고정 */
    return HTTP_CONN_WANT_BACKEND_WRITE;
}

/* 연결 완료는 현재 백엔드의 쓰기 준비·ERR 알림을 받은 차례에만 SO_ERROR로 판정한다(D-67). */
static int on_px_connect(http_conn *c, turn *t, long long now)
{
    int err = 0;
    socklen_t len = sizeof(err);

    if (!(t->wake & (HTTP_CONN_WAKE_BACKEND_OUT | HTTP_CONN_WAKE_BACKEND_ERR))) {
        return wait_px(c, WAIT_BE_CONNECT, now);
    }
    t->wake = 0;
    if (getsockopt(c->be_fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        return px_fail(c, 502);
    }
    start_upload(c);
    return CONTINUE;
}

/* D-65 커밋: 다시 쓴 머리와 미리 받은 본문(길이까지)을 출력 버퍼에 채운다. 그 뒤에는 502·504로 바꾸지 않는다. */
static int px_commit(http_conn *c, const proxy_response *resp, long long now)
{
    int keep = resp->body != PROXY_BODY_EOF && c->close_after == 0;
    size_t extra = c->be_in_len - resp->head_len;
    int n;

    if (!keep) {
        c->close_after = 1;
    }
    n = proxy_format_response_head(c->out, sizeof(c->out), c->be_in, resp, keep ? c->conn_hdr : HTTP_CONNECTION_CLOSE);
    if (n < 0) {
        return finish(c, HTTP_CONN_ERROR, errno); /* 머리 8KB → 출력 64KB: 오지 않는 경로 */
    }
    c->resp_body = resp->body;
    c->resp_left = 0;
    if (resp->body == PROXY_BODY_NONE) {
        extra = 0; /* 204·304 뒤 바이트는 내보내지 않는다 */
    } else if (resp->body == PROXY_BODY_LENGTH) {
        if (extra > resp->content_length) {
            extra = resp->content_length; /* 넘는 바이트는 버린다 */
        }
        c->resp_left = resp->content_length - extra;
    }
    memcpy(c->out + n, c->be_in + resp->head_len, extra);
    filled(c, (size_t)n + extra, now);
    c->committed = 1;
    c->uploading = 0;
    c->be_out_pos = c->be_out_len = 0; /* 조기 응답: 미송신 바이트 버림(D-66) */
    c->be_in_len = 0;
    if (resp->body == PROXY_BODY_NONE || (resp->body == PROXY_BODY_LENGTH && c->resp_left == 0)) {
        close_backend(c); /* 응답 끝: 백엔드 EOF를 기다리지 않는다 */
    }
    c->state = ST_PX_BODY;
    return CONTINUE;
}

#define NOTHING (-2) /* 응답 머리가 아직 완성되지 않음 */

/* 백엔드에서 응답 머리를 한 번 읽는다(소켓 read 예산 1번). 완성·실패면 상태를 바꾸고 CONTINUE. */
static int read_head(http_conn *c, turn *t, long long now)
{
    proxy_response resp;
    ssize_t n;

    t->did_read = 1;
    n = read(c->be_fd, c->be_in + c->be_in_len, sizeof(c->be_in) - c->be_in_len);
    if (n > 0) {
        c->be_in_len += (size_t)n;
        if (!c->uploading) {
            c->deadline = now + HTTP_CONN_TIMEOUT_NS; /* 응답 머리 대기는 읽기 진행으로 잰다 */
        }
        switch (proxy_parse_response(c->be_in, c->be_in_len, &resp)) {
        case PROXY_PARSE_OK: return px_commit(c, &resp, now);
        case PROXY_PARSE_BAD: return px_fail(c, 502);
        default: return NOTHING;
        }
    }
    if (n < 0 && would_block(errno)) {
        return NOTHING;
    }
    return px_fail(c, 502); /* 머리 없이 EOF·읽기 오류(D-66) */
}

/*
 * 업로드: 백엔드 알림이면 먼저 응답(조기 응답)을 읽고, 백엔드 버퍼를 보내고, 비면 클라이언트 본문을 남은 길이만큼 읽는다.
 * 쓰기 실패는 업로드를 멈추고 응답 머리를 기다린다(D-66, D-67).
 */
static int on_px_upload(http_conn *c, turn *t, long long now)
{
    ssize_t n;

    if ((t->wake & (HTTP_CONN_WAKE_BACKEND_IN | HTTP_CONN_WAKE_BACKEND_ERR)) && !t->did_read) {
        int r;
        t->wake = 0;
        r = read_head(c, t, now);
        if (r != NOTHING) {
            return r;
        }
    }
    if (c->be_out_pos < c->be_out_len) {
        if (t->did_be_write) {
            return wait_px(c, WAIT_BE_WRITE, now);
        }
        t->did_be_write = 1;
        n = write(c->be_fd, c->be_out + c->be_out_pos, c->be_out_len - c->be_out_pos);
        if (n > 0) {
            c->be_out_pos += (size_t)n;
            c->deadline = now + HTTP_CONN_TIMEOUT_NS;
            return CONTINUE;
        }
        if (n < 0 && would_block(errno)) {
            return wait_px(c, WAIT_BE_WRITE, now);
        }
        c->uploading = 0; /* EPIPE·ECONNRESET 등: 업로드를 멈추고 응답을 기다린다 */
        c->be_out_pos = c->be_out_len = 0;
        c->state = ST_PX_HEAD;
        return CONTINUE;
    }
    c->be_out_pos = c->be_out_len = 0;
    if (c->body_left == 0) {
        c->uploading = 0;
        c->state = ST_PX_HEAD;
        return CONTINUE;
    }
    if (t->did_read) {
        return wait_px(c, WAIT_READ, now);
    }
    t->did_read = 1;
    n = read(c->fd, c->be_out, c->body_left < sizeof(c->be_out) ? c->body_left : sizeof(c->be_out));
    if (n > 0) {
        c->be_out_len = (size_t)n;
        c->body_left -= (size_t)n;
        c->deadline = now + HTTP_CONN_TIMEOUT_NS;
        return CONTINUE;
    }
    if (n < 0 && would_block(errno)) {
        return wait_px(c, WAIT_READ, now);
    }
    return mid_request_failure(c, n);
}

static int on_px_head(http_conn *c, turn *t, long long now)
{
    int r;

    if (t->did_read) {
        return wait_px(c, WAIT_BE_READ, now);
    }
    r = read_head(c, t, now);
    return r == NOTHING ? wait_px(c, WAIT_BE_READ, now) : r;
}

/* 커밋 뒤 본문 조각: 길이 있는 응답은 남은 길이보다 더 읽지 않는다. 길이 전 EOF·오류는 응답 없이 닫는다(D-65, D-66). */
static int on_px_body(http_conn *c, turn *t, long long now)
{
    size_t want = sizeof(c->out);
    ssize_t n;

    if (c->be_fd < 0) { /* 응답 끝 */
        c->state = ST_PX_SKIP;
        return CONTINUE;
    }
    if (t->did_read) {
        return wait_px(c, WAIT_BE_READ, now);
    }
    t->did_read = 1;
    if (c->resp_body == PROXY_BODY_LENGTH && c->resp_left < want) {
        want = c->resp_left;
    }
    n = read(c->be_fd, c->out, want);
    if (n > 0) {
        if (c->resp_body == PROXY_BODY_LENGTH) {
            c->resp_left -= (size_t)n;
            if (c->resp_left == 0) {
                close_backend(c);
            }
        }
        filled(c, (size_t)n, now);
        return CONTINUE;
    }
    if (n < 0 && would_block(errno)) {
        return wait_px(c, WAIT_BE_READ, now);
    }
    if (n == 0 && c->resp_body == PROXY_BODY_EOF) {
        close_backend(c); /* 길이 없는 응답의 정상 끝 */
        return CONTINUE;
    }
    return finish(c, HTTP_CONN_ERROR, EIO);
}

/* 남은 요청 본문을 버린다(이미 받은 본문은 다시 읽지 않는다, D-66). 다 버리면 저장한 502·504 또는 유지·닫기 판단. */
static int on_px_skip(http_conn *c, turn *t, long long now)
{
    ssize_t n;

    if (c->body_left > 0 && c->pending_status == 0 && c->close_after != 0) {
        c->state = ST_RESPONDED; /* 닫기 규칙이면 남은 본문을 버리지 않고 닫는다 */
        return CONTINUE;
    }
    if (c->body_left == 0) {
        if (c->pending_status != 0) {
            http_response_extra extra = {NULL, NULL, c->conn_hdr};
            int status = c->pending_status;
            c->pending_status = 0;
            return fill_status(c, status, &extra, now);
        }
        c->state = ST_RESPONDED;
        return CONTINUE;
    }
    if (t->did_read) {
        return wait_px(c, WAIT_READ, now);
    }
    t->did_read = 1;
    n = read(c->fd, c->in, c->body_left < sizeof(c->in) ? c->body_left : sizeof(c->in));
    if (n > 0) {
        c->body_left -= (size_t)n;
        c->deadline = now + HTTP_CONN_TIMEOUT_NS;
        return CONTINUE;
    }
    if (n < 0 && would_block(errno)) {
        return wait_px(c, WAIT_READ, now);
    }
    return mid_request_failure(c, n);
}

static const state_fn handlers[] = {on_head,   on_body_start, on_echo,      on_discard, on_file,   on_responded,
                                    on_drain,  on_px_connect, on_px_upload, on_px_head, on_px_body, on_px_skip};

void http_conn_init(http_conn *c, int fd, const char *root, route_table *routes, long long now)
{
    c->fd = fd;
    c->root = root;
    c->routes = routes;
    c->be_fd = -1;
    c->be_gen = 0;
    c->uploading = c->committed = c->pending_status = 0;
    c->be_out_pos = c->be_out_len = c->be_in_len = 0;
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

http_conn_step http_conn_run(http_conn *c, int wake, long long now)
{
    turn t = {0, 0, c->out_len - c->out_pos, 0, wake};

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

http_conn_step http_conn_expire(http_conn *c)
{
    switch (c->wait) {
    case WAIT_READ: return (http_conn_step)finish(c, HTTP_CONN_TIMED_OUT, EAGAIN);
    case WAIT_WRITE: return (http_conn_step)finish(c, HTTP_CONN_ERROR, EAGAIN);
    case WAIT_BE_READ:
        if (c->committed) {
            return (http_conn_step)finish(c, HTTP_CONN_ERROR, ETIMEDOUT); /* 커밋 뒤: 응답 없이 닫기 */
        }
        /* fall through - 커밋 전 백엔드 만료는 504 */
    case WAIT_BE_CONNECT:
    case WAIT_BE_WRITE:
        px_fail(c, 504);
        return HTTP_CONN_RUNNABLE; /* D-67: 루프는 끝으로 보지 않고 내부 실행 목록에 넣는다 */
    default: return (http_conn_step)finish(c, HTTP_CONN_DONE, 0); /* 요청 사이·정리 */
    }
}

void http_conn_release(http_conn *c)
{
    if (c->file_fd >= 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
    close_backend(c);
}
