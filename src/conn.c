/*
 * 연결 하나의 읽기·보내기: docs/design/step5.md 상태 전이
 * 할 수 있는 만큼만 읽고 보내고 돌아와요. 응답 내용은 respond.c가 준비해요.
 */
#include "server.h"

#include "http.h"
#include "log.h"
#include "proxy.h"
#include "respond.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#define FILE_BUF_SIZE 8192

void conn_watch_client(struct server *s, struct conn *c, unsigned events)
{
    if (c->cli_events == events)
        return;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = &c->h_cli;
    if (epoll_ctl(s->ep, EPOLL_CTL_MOD, c->fd, &ev) < 0)
        perror("epoll_ctl MOD");
    c->cli_events = events;
}

/* 관심 목록에서 이 연결이 지켜볼 일을 바꿔요 (EPOLLIN 또는 EPOLLOUT) */
static void watch(struct server *s, struct conn *c, unsigned events)
{
    conn_watch_client(s, c, events);
}

/* 본문 길이를 정해요. 0: c->cl에 길이, 그 밖: 경계를 알 수 없어 보낼 상태 코드 (400·411·413·501) */
static int body_length(struct conn *c)
{
    if (http_find_header(&c->req, "Transfer-Encoding") != NULL)
        return 501;
    int r = http_content_length(&c->req, &c->cl);
    if (r < 0)
        return 400;
    if (r == 0 && strcmp(c->req.method, "POST") == 0)
        return 411;
    if (c->cl > HTTP_MAX_BODY)
        return 413;
    return 0;
}

/* ---- 보내기 ---- */

enum { FLUSH_DONE, FLUSH_WAIT, FLUSH_CLOSED };

/* out, 그다음 파일을 보낼 수 있는 만큼 보내요.
   FLUSH_DONE: 다 보냄(keep이면 다음 요청 준비됨), FLUSH_WAIT: 보내기 버퍼가 참, FLUSH_CLOSED: 연결을 닫음(c 해제됨) */
static int flush(struct server *s, struct conn *c)
{
    char fbuf[FILE_BUF_SIZE];
    for (;;) {
        const char *p;
        size_t n;
        if (c->out_sent < c->out_len) {
            p = c->out + c->out_sent;
            n = c->out_len - c->out_sent;
        } else if (c->file_fd >= 0 && c->file_off < c->file_size) {
            ssize_t r = pread(c->file_fd, fbuf, sizeof(fbuf), c->file_off);
            if (r <= 0) {
                if (r < 0 && errno == EINTR)
                    continue;
                perror("pread");
                server_close_conn(s, c);     /* 헤더는 이미 나감: 닫기만 */
                return FLUSH_CLOSED;
            }
            p = fbuf;
            n = (size_t)r;
        } else {
            break;                           /* 다 보냄 */
        }

        ssize_t w = write(c->fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!c->want_out) {
                    watch(s, c, EPOLLOUT);   /* 자리가 나면 알려 줘 */
                    c->want_out = 1;
                }
                return FLUSH_WAIT;
            }
            server_close_conn(s, c);         /* EPIPE, ECONNRESET: 클라이언트가 떠남 */
            return FLUSH_CLOSED;
        }
        c->last_active = time(NULL);
        c->bytes_out += (size_t)w;
        if (c->out_sent < c->out_len)
            c->out_sent += (size_t)w;
        else
            c->file_off += w;                /* 실제로 쓴 만큼만 (S5-05) */
    }

    if (c->want_out) {
        watch(s, c, EPOLLIN);
        c->want_out = 0;
    }
    access_log_conn(c);                      /* 응답 하나 끝: 로그 한 줄 (S7-06) */
    response_reset(c);
    if (!c->keep || s->stopping) {           /* 종료 중이면 keep-alive 없이 닫아요 (S7-02) */
        server_close_conn(s, c);
        return FLUSH_CLOSED;
    }
    /* 이 요청이 쓴 바이트를 빼고 남은 바이트(다음 요청)를 앞으로 당겨요 */
    memmove(c->buf, c->buf + c->hdr_end, c->len - c->hdr_end);
    c->len -= c->hdr_end;
    c->hdr_end = 0;
    c->state = ST_HEADER;
    if (c->len > 0)
        c->req_start = time(NULL);           /* 다음 요청이 이미 와 있음: 헤더 시계 시작 */
    return FLUSH_DONE;
}

/* 응답을 준비해 보내기 시작해요 */
static int start_write(struct server *s, struct conn *c)
{
    c->state = ST_WRITE;
    if (c->status == 0 && c->out != NULL && c->out_len >= 12)
        c->status = atoi(c->out + 9);        /* "HTTP/1.1 200 ..." */
    c->log_pending = 1;
    return flush(s, c);
}

/* ---- 읽기 ---- */

/* 버퍼에 있는 것으로 할 수 있는 만큼 하고, 모자라면 더 읽기를 EAGAIN까지 반복해요 */
static void advance(struct server *s, struct conn *c)
{
    for (;;) {
        char *dst;
        size_t room;

        if (c->state == ST_HEADER) {
            size_t end = http_find_header_end(c->buf, c->len);
            if (end == 0 && c->len == sizeof(c->buf))
                end = (size_t)-1;            /* 버퍼가 꽉 찼는데 헤더의 끝이 없음 */
            if (end != 0) {
                clock_gettime(CLOCK_MONOTONIC, &c->t_req);
                c->status = 0;
                c->bytes_out = 0;
                int status = end == (size_t)-1 ? 431 : http_parse_request(c->buf, end, &c->req);
                c->has_req = status == 0;
                if (status == 0)
                    status = body_length(c);
                if (status != 0) {           /* 경계를 모름: 오류 응답 뒤 닫기 */
                    c->keep = 0;
                    c->hdr_end = c->len;
                    if (prepare_error(c, status, NULL) < 0) {
                        server_close_conn(s, c);
                        return;
                    }
                    start_write(s, c);
                    return;
                }
                c->keep = http_keep_alive(&c->req);
                if (s->stopping)
                    c->keep = 0;                 /* 종료 중: 이 응답 뒤에 닫아요 (S7-02) */
                size_t have = c->len - end;
                if (have > c->cl)
                    have = c->cl;            /* 그 뒤는 다음 요청 */
                c->hdr_end = end + have;     /* 이 요청이 헤더 버퍼에서 쓴 바이트 수 */
                if (c->cl > 0) {
                    c->body = malloc(c->cl);
                    if (c->body == NULL) {
                        perror("malloc");
                        server_close_conn(s, c);
                        return;
                    }
                    memcpy(c->body, c->buf + end, have);
                    c->body_len = have;
                }
                c->state = ST_BODY;
                continue;
            }
            dst = c->buf + c->len;
            room = sizeof(c->buf) - c->len;
        } else if (c->state == ST_BODY) {
            if (c->body_len == c->cl) {
                struct route *rt = s->routes != NULL ? routes_match(s->routes, c->req.path) : NULL;
                if (rt != NULL) {                /* 라우팅 표에 맞으면 백엔드로 (S6-02) */
                    proxy_start(s, c, rt);
                    return;
                }
                if (prepare_response(s, c) < 0) {
                    server_close_conn(s, c);
                    return;
                }
                if (start_write(s, c) != FLUSH_DONE)
                    return;
                continue;                    /* 다음 요청이 이미 버퍼에 있을 수 있음 (파이프라이닝) */
            }
            dst = c->body + c->body_len;
            room = c->cl - c->body_len;
        } else {
            return;                          /* 보내는 중에는 읽지 않아요 */
        }

        ssize_t n = read(c->fd, dst, room);
        if (n > 0) {
            c->last_active = time(NULL);
            if (c->state == ST_HEADER && c->len == 0)
                c->req_start = c->last_active;   /* 새 요청의 첫 바이트: 헤더 시계 시작 (S7-04) */
            if (c->state == ST_HEADER)
                c->len += (size_t)n;
            else
                c->body_len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;                          /* 지금은 없음: 다음 알림까지 손 뗌 */
        if (n < 0)
            perror("read");
        server_close_conn(s, c);             /* 0: 클라이언트가 끊음, 그 밖: 실패 */
        return;
    }
}

/* ---- 밖에 여는 함수 ---- */

void conn_send_error(struct server *s, struct conn *c, int status)
{
    response_reset(c);
    c->keep = 0;
    c->hdr_end = c->len;                         /* 뒤에 붙은 요청은 버려요 */
    c->want_out = 0;
    c->status = 0;
    c->bytes_out = 0;
    conn_watch_client(s, c, EPOLLIN);
    if (prepare_error(c, status, NULL) < 0) {
        server_close_conn(s, c);
        return;
    }
    start_write(s, c);
}

void conn_on_readable(struct server *s, struct conn *c)
{
    advance(s, c);
}

void conn_on_writable(struct server *s, struct conn *c)
{
    if (c->state != ST_WRITE)
        return;
    if (flush(s, c) == FLUSH_DONE && c->len > 0)
        advance(s, c);                       /* 다음 요청이 이미 버퍼에 있음 */
}
