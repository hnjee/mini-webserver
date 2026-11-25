/*
 * 서버: docs/design/step1.md ~ step5.md
 * 리슨 소켓, epoll과 메인 루프, 연결 받기와 정리를 맡아요 (Step 5).
 * 연결 하나의 읽기·보내기는 conn.c, 응답 만들기는 respond.c에 있어요.
 */
#include "server.h"

#include "respond.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DRAIN_LIMIT (16 * 1024)

int server_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        perror("fcntl O_NONBLOCK");
}

int server_init(struct server *s, int listen_fd, const char *root)
{
    memset(s, 0, sizeof(*s));
    s->listen_fd = listen_fd;
    s->root = root;
    s->ep = epoll_create1(0);
    if (s->ep < 0) {
        perror("epoll_create1");
        return -1;
    }
    set_nonblock(listen_fd);
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;                      /* NULL이면 리슨 소켓 */
    if (epoll_ctl(s->ep, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl ADD listen");
        close(s->ep);
        s->ep = -1;
        return -1;
    }
    return 0;
}

int server_on_accept(struct server *s)
{
    int count = 0;
    for (int i = 0; i < ACCEPT_BATCH; i++) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;                       /* 줄이 빔 */
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            break;
        }
        set_nonblock(fd);

        struct conn *c = calloc(1, sizeof(*c));
        if (c == NULL) {
            perror("calloc");
            close(fd);
            continue;
        }
        c->fd = fd;
        c->file_fd = -1;
        c->state = ST_HEADER;
        c->last_active = time(NULL);

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = c;                     /* 알림이 오면 이 conn을 돌려받아요 */
        if (epoll_ctl(s->ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("epoll_ctl ADD");
            close(fd);
            free(c);
            continue;
        }
        c->next = s->conns;
        if (s->conns != NULL)
            s->conns->prev = c;
        s->conns = c;
        s->nconn++;
        count++;
    }
    return count;
}



void server_close_conn(struct server *s, struct conn *c)
{
    if (epoll_ctl(s->ep, EPOLL_CTL_DEL, c->fd, NULL) < 0)
        perror("epoll_ctl DEL");
    /* 쓰기를 먼저 닫고 남은 데이터를 한 번 읽어 버려요. close()에서 RST가 나가지 않게. 논블로킹이라 기다리지 않아요 */
    shutdown(c->fd, SHUT_WR);
    char tmp[DRAIN_LIMIT];
    while (read(c->fd, tmp, sizeof(tmp)) < 0 && errno == EINTR)
        ;
    if (close(c->fd) < 0)
        perror("close");
    response_reset(c);

    if (c->prev != NULL)
        c->prev->next = c->next;
    else
        s->conns = c->next;
    if (c->next != NULL)
        c->next->prev = c->prev;
    s->nconn--;
    free(c);
}

int server_sweep_idle(struct server *s, time_t now)
{
    int closed = 0;
    struct conn *c = s->conns;
    while (c != NULL) {
        struct conn *next = c->next;
        if (now - c->last_active >= IDLE_TIMEOUT_SEC) {
            server_close_conn(s, c);
            closed++;
        }
        c = next;
    }
    return closed;
}

void server_free(struct server *s)
{
    while (s->conns != NULL)
        server_close_conn(s, s->conns);
    if (s->ep >= 0)
        close(s->ep);
    s->ep = -1;
}

void server_run(int listen_fd, const char *root_real)
{
    struct server s;
    if (server_init(&s, listen_fd, root_real) < 0)
        return;

    struct epoll_event evs[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(s.ep, evs, MAX_EVENTS, TICK_MS);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            struct conn *c = evs[i].data.ptr;
            if (c == NULL)
                server_on_accept(&s);                       /* 손님 받기 */
            else if (evs[i].events & (EPOLLHUP | EPOLLERR))
                server_close_conn(&s, c);
            else if (c->state == ST_WRITE)
                conn_on_writable(&s, c);                    /* 이어서 보내기 */
            else
                conn_on_readable(&s, c);                    /* 이어서 읽기 */
        }
        server_sweep_idle(&s, time(NULL));                  /* 15초 넘은 연결 닫기 */
    }
    server_free(&s);
}
