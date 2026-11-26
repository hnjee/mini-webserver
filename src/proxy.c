/*
 * 리버스 프록시: docs/design/step6.md 4장 (S6-03 ~ S6-10)
 * 연결 하나에 클라이언트 소켓과 백엔드 소켓 두 개를 들고, 백엔드 응답을 64KB 버퍼로 받는 대로 넘겨요.
 * 백엔드 소켓도 epoll에 등록해서, 기다리는 동안 다른 연결을 막지 않아요 (S6-08).
 */
#include "proxy.h"

#include "respond.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

int proxy_active(const struct conn *c)
{
    return c->state == ST_PROXY_CONNECT || c->state == ST_PROXY_SEND || c->state == ST_PROXY_RELAY;
}

/* 백엔드 소켓이 지켜볼 일을 바꿔요. 같으면 그대로 둬요 */
static void watch_back(struct server *s, struct conn *c, unsigned events)
{
    if (c->back_fd < 0 || c->back_events == events)
        return;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = &c->h_back;
    if (epoll_ctl(s->ep, EPOLL_CTL_MOD, c->back_fd, &ev) < 0)
        perror("epoll_ctl MOD backend");
    c->back_events = events;
}

void proxy_cleanup(struct server *s, struct conn *c)
{
    if (c->back_fd >= 0) {
        if (epoll_ctl(s->ep, EPOLL_CTL_DEL, c->back_fd, NULL) < 0)
            perror("epoll_ctl DEL backend");
        close(c->back_fd);
        c->back_fd = -1;
        c->back_events = 0;
    }
    free(c->relay);
    c->relay = NULL;
    c->rpos = c->wpos = 0;
}

/* 아직 클라이언트에 아무것도 안 보냈으면 status(502·504)를 보내고, 보냈으면 닫아요 */
static void fail(struct server *s, struct conn *c, int status)
{
    proxy_cleanup(s, c);
    if (c->sent_to_client == 0)
        conn_send_error(s, c, status);
    else
        server_close_conn(s, c);
}

/* 백엔드에 보낼 요청을 c->out에 만들어요 (S6-03). 헤더는 그대로, Connection·Keep-Alive만 빼고 Connection: close */
static int build_request(struct conn *c)
{
    const struct http_request *r = &c->req;
    size_t cap = strlen(r->method) + strlen(r->path) + strlen(r->version) + 64 + c->body_len;
    for (int i = 0; i < r->header_count; i++)
        cap += strlen(r->headers[i].name) + strlen(r->headers[i].value) + 4;
    char *out = malloc(cap);
    if (out == NULL)
        return -1;

    size_t n = (size_t)snprintf(out, cap, "%s %s %s\r\n", r->method, r->path, r->version);
    for (int i = 0; i < r->header_count; i++) {
        const char *name = r->headers[i].name;
        if (strcasecmp(name, "Connection") == 0 || strcasecmp(name, "Keep-Alive") == 0)
            continue;
        n += (size_t)snprintf(out + n, cap - n, "%s: %s\r\n", name, r->headers[i].value);
    }
    n += (size_t)snprintf(out + n, cap - n, "Connection: close\r\n\r\n");
    if (c->body_len > 0) {
        memcpy(out + n, c->body, c->body_len);
        n += c->body_len;
    }
    free(c->body);
    c->body = NULL;
    c->body_len = 0;

    c->out = out;
    c->out_len = n;
    c->out_sent = 0;
    return 0;
}

void proxy_start(struct server *s, struct conn *c, struct route *rt)
{
    c->keep = 0;                                 /* 프록시 응답 뒤에는 클라이언트도 닫아요 (결정 20행) */
    c->log_pending = 1;                          /* 끝나거나 끊길 때 로그를 남겨요 (S7-06) */
    c->hdr_end = c->len;                         /* 뒤에 붙은 요청은 버려요 */
    c->sent_to_client = 0;
    c->back_eof = 0;
    c->cli_halfclosed = 0;
    if (build_request(c) < 0) {
        server_close_conn(s, c);
        return;
    }

    const struct backend *b = route_pick(rt);     /* 라운드 로빈 (S6-05) */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        conn_send_error(s, c, 502);
        return;
    }
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    time_t now = time(NULL);
    if (connect(fd, (const struct sockaddr *)&b->addr, sizeof(b->addr)) == 0) {
        c->state = ST_PROXY_SEND;
        c->deadline = now + s->response_timeout;
    } else if (errno == EINPROGRESS) {
        c->state = ST_PROXY_CONNECT;
        c->deadline = now + s->connect_timeout;
    } else {
        close(fd);                               /* 거절 등: 다시 시도하지 않고 502 (S6-06) */
        conn_send_error(s, c, 502);
        return;
    }

    c->back_fd = fd;
    c->h_back.kind = H_BACKEND;
    c->h_back.c = c;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT;                        /* 연결되면(쓸 수 있으면) 알려 줘 */
    ev.data.ptr = &c->h_back;
    if (epoll_ctl(s->ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl ADD backend");
        close(fd);
        c->back_fd = -1;
        conn_send_error(s, c, 502);
        return;
    }
    c->back_events = EPOLLOUT;
    conn_watch_client(s, c, EPOLLRDHUP);        /* 클라이언트는 끊김만 지켜봐요 */
}

/* 클라이언트에 보낼 수 있는 만큼 보내요. 연결을 닫았으면 -1 */
static int relay_flush(struct server *s, struct conn *c)
{
    if (c->status == 0 && c->sent_to_client == 0 && c->wpos - c->rpos >= 12 &&
        memcmp(c->relay + c->rpos, "HTTP/", 5) == 0)
        c->status = atoi(c->relay + c->rpos + 9);   /* 백엔드 응답의 상태 코드 (로그용) */
    while (c->rpos < c->wpos) {
        ssize_t w = write(c->fd, c->relay + c->rpos, c->wpos - c->rpos);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            server_close_conn(s, c);             /* EPIPE, ECONNRESET: 클라이언트가 떠남 (S6-10) */
            return -1;
        }
        c->rpos += (size_t)w;
        c->sent_to_client += (size_t)w;
        c->bytes_out += (size_t)w;
        c->last_active = time(NULL);
    }
    if (c->rpos == c->wpos)
        c->rpos = c->wpos = 0;
    else if (c->wpos == PROXY_RELAY_SIZE && c->rpos > 0) {
        memmove(c->relay, c->relay + c->rpos, c->wpos - c->rpos);   /* 앞이 비었으면 당겨요 */
        c->wpos -= c->rpos;
        c->rpos = 0;
    }
    return 0;
}

/* 백엔드에서 읽고 클라이언트에 넘기기를 할 수 있는 만큼 해요 (S6-04) */
static void relay_pump(struct server *s, struct conn *c)
{
    for (;;) {
        int progressed = 0;
        while (!c->back_eof && c->wpos < PROXY_RELAY_SIZE) {
            ssize_t n = read(c->back_fd, c->relay + c->wpos, PROXY_RELAY_SIZE - c->wpos);
            if (n > 0) {
                c->wpos += (size_t)n;
                c->deadline = time(NULL) + s->response_timeout;
                progressed = 1;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (n < 0)
                perror("read backend");
            c->back_eof = 1;                     /* 0: 백엔드가 닫음 = 응답 끝, 그 밖: 실패 */
        }
        size_t before = c->sent_to_client;
        if (relay_flush(s, c) < 0)
            return;
        if (c->sent_to_client != before)
            progressed = 1;
        if (c->back_eof || !progressed || c->wpos == PROXY_RELAY_SIZE)
            break;
    }

    if (c->back_eof && c->rpos == c->wpos) {     /* 끝내기 */
        if (c->sent_to_client == 0) {
            fail(s, c, 502);                     /* 한 바이트도 없이 닫힘 (S6-10) */
        } else {
            proxy_cleanup(s, c);
            server_close_conn(s, c);             /* 백엔드가 닫으면 클라이언트도 닫아요 (S6-04) */
        }
        return;
    }
    if (c->back_eof && c->back_fd >= 0) {        /* 백엔드는 끝났고 클라이언트로 보낼 것만 남음 */
        if (epoll_ctl(s->ep, EPOLL_CTL_DEL, c->back_fd, NULL) < 0)
            perror("epoll_ctl DEL backend");
        close(c->back_fd);
        c->back_fd = -1;
        c->back_events = 0;
    }
    /* 버퍼에 자리가 있으면 백엔드를 읽고, 꽉 차면 멈춰요. 보낼 게 있으면 클라이언트 EPOLLOUT */
    watch_back(s, c, (!c->back_eof && c->wpos < PROXY_RELAY_SIZE) ? EPOLLIN : 0);
    conn_watch_client(s, c, (c->cli_halfclosed ? 0 : EPOLLRDHUP) | (c->rpos < c->wpos ? EPOLLOUT : 0));
}

/* 요청을 보낼 수 있는 만큼 보내요 */
static void send_request(struct server *s, struct conn *c)
{
    while (c->out_sent < c->out_len) {
        ssize_t w = write(c->back_fd, c->out + c->out_sent, c->out_len - c->out_sent);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;                          /* EPOLLOUT을 기다려요 */
            fail(s, c, 502);
            return;
        }
        c->out_sent += (size_t)w;
        c->deadline = time(NULL) + s->response_timeout;
    }
    free(c->out);                                /* 요청은 다 보냈어요 */
    c->out = NULL;
    c->out_len = c->out_sent = 0;
    c->relay = malloc(PROXY_RELAY_SIZE);
    if (c->relay == NULL) {
        perror("malloc relay");
        fail(s, c, 502);
        return;
    }
    c->rpos = c->wpos = 0;
    c->state = ST_PROXY_RELAY;
    c->deadline = time(NULL) + s->response_timeout;
    watch_back(s, c, EPOLLIN);
}

void proxy_on_backend(struct server *s, struct conn *c, unsigned events)
{
    if (c->state == ST_PROXY_CONNECT) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(c->back_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            fail(s, c, 502);                     /* 연결 실패 (S6-06) */
            return;
        }
        c->state = ST_PROXY_SEND;
        c->deadline = time(NULL) + s->response_timeout;
    }
    if (c->state == ST_PROXY_SEND) {
        if (events & EPOLLERR) {
            fail(s, c, 502);
            return;
        }
        send_request(s, c);
        return;
    }
    if (c->state == ST_PROXY_RELAY)
        relay_pump(s, c);
}

void proxy_on_client(struct server *s, struct conn *c, unsigned events)
{
    if (events & (EPOLLHUP | EPOLLERR)) {
        server_close_conn(s, c);                 /* 클라이언트가 떠남: 양쪽 정리 (S6-10) */
        return;
    }
    if (events & EPOLLRDHUP) {
        /* 쓰기만 닫은 클라이언트(half-close)일 수 있어서 바로 끊지 않아요. 더는 지켜보지 않고, 보내다 실패하면 닫아요 */
        c->cli_halfclosed = 1;
        conn_watch_client(s, c, c->cli_events & ~(unsigned)EPOLLRDHUP);
    }
    if ((events & EPOLLOUT) && c->state == ST_PROXY_RELAY)
        relay_pump(s, c);
}

int proxy_check_timeout(struct server *s, struct conn *c, time_t now)
{
    if (!proxy_active(c) || now < c->deadline)
        return 0;
    fprintf(stderr, "proxy: 백엔드 시간 제한 (%s)\n", c->state == ST_PROXY_CONNECT ? "연결" : "응답");
    fail(s, c, 504);                             /* 보낸 것 없으면 504, 있으면 닫기 (S6-07) */
    return 1;
}
