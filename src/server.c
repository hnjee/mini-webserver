/*
 * 서버: docs/design/step1.md ~ step6.md
 * 리슨 소켓, epoll과 메인 루프, 연결 받기와 정리, 워커 스레드를 맡아요 (Step 5, 6).
 * 연결 하나의 읽기·보내기는 conn.c, 응답 만들기는 respond.c, 프록시는 proxy.c에 있어요.
 */
#include "server.h"

#include "proxy.h"
#include "respond.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
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
    s->h_listen.kind = H_LISTEN;
    s->h_listen.c = NULL;
    s->connect_timeout = PROXY_CONNECT_TIMEOUT_SEC;
    s->response_timeout = PROXY_RESPONSE_TIMEOUT_SEC;
    s->ep = epoll_create1(0);
    if (s->ep < 0) {
        perror("epoll_create1");
        return -1;
    }
    set_nonblock(listen_fd);
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLEXCLUSIVE;        /* 워커 여럿이 같이 지켜봐도 손님 하나에 하나만 깨워요 (S6-01) */
    ev.data.ptr = &s->h_listen;
    if (epoll_ctl(s->ep, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        ev.events = EPOLLIN;                     /* 오래된 커널: EPOLLEXCLUSIVE 없이 */
        if (epoll_ctl(s->ep, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
            perror("epoll_ctl ADD listen");
            close(s->ep);
            s->ep = -1;
            return -1;
        }
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
        c->back_fd = -1;
        c->state = ST_HEADER;
        c->last_active = time(NULL);
        c->h_cli.kind = H_CLIENT;
        c->h_cli.c = c;
        c->h_back.kind = H_BACKEND;
        c->h_back.c = c;
        c->cli_events = EPOLLIN;

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = &c->h_cli;             /* 알림이 오면 이 손잡이로 conn을 찾아요 */
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
    if (c->closed)
        return;
    proxy_cleanup(s, c);                     /* 백엔드 소켓과 중계 버퍼 (S6-10) */
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

    /* 같은 바퀴의 다른 알림(백엔드 소켓 등)이 이 conn을 가리킬 수 있어서, 바퀴가 끝날 때 해제해요 */
    c->closed = 1;
    c->prev = NULL;
    c->next = s->dead;
    s->dead = c;
}

/* 이번 바퀴에 닫힌 conn을 해제해요 */
static void reap(struct server *s)
{
    while (s->dead != NULL) {
        struct conn *c = s->dead;
        s->dead = c->next;
        free(c);
    }
}

int server_sweep_idle(struct server *s, time_t now)
{
    int closed = 0;
    struct conn *c = s->conns;
    while (c != NULL) {
        struct conn *next = c->next;
        if (proxy_active(c)) {
            closed += proxy_check_timeout(s, c, now);   /* 백엔드 시간 제한 (S6-07) */
        } else if (now - c->last_active >= IDLE_TIMEOUT_SEC) {
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
    reap(s);
    if (s->ep >= 0)
        close(s->ep);
    s->ep = -1;
}

int server_poll_once(struct server *s, int timeout_ms)
{
    struct epoll_event evs[MAX_EVENTS];
    int n = epoll_wait(s->ep, evs, MAX_EVENTS, timeout_ms);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        perror("epoll_wait");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        struct handle *h = evs[i].data.ptr;
        unsigned events = evs[i].events;
        if (h->kind == H_LISTEN) {
            server_on_accept(s);                            /* 손님 받기 */
            continue;
        }
        struct conn *c = h->c;
        if (c->closed)
            continue;                                       /* 이번 바퀴에 이미 닫힘 */
        if (h->kind == H_BACKEND)
            proxy_on_backend(s, c, events);                 /* 백엔드 쪽 (Step 6) */
        else if (proxy_active(c))
            proxy_on_client(s, c, events);                  /* 프록시 중인 클라이언트 쪽 */
        else if (events & (EPOLLHUP | EPOLLERR))
            server_close_conn(s, c);
        else if (c->state == ST_WRITE)
            conn_on_writable(s, c);                         /* 이어서 보내기 */
        else
            conn_on_readable(s, c);                         /* 이어서 읽기 */
    }
    reap(s);
    return n;
}

/* 워커 하나: 자기 epoll로 Step 5 루프를 돌려요 */
struct worker_arg {
    int listen_fd;
    const char *root;
    struct routes *routes;
};

static void *worker_main(void *p)
{
    const struct worker_arg *a = p;
    struct server s;
    if (server_init(&s, a->listen_fd, a->root) < 0)
        return NULL;
    s.routes = a->routes;
    for (;;) {
        if (server_poll_once(&s, TICK_MS) < 0)
            break;
        server_sweep_idle(&s, time(NULL));                  /* 15초 넘은 연결, 백엔드 시간 제한 */
    }
    server_free(&s);
    return NULL;
}

void server_run(int listen_fd, const char *root_real, struct routes *routes, int nworkers)
{
    if (nworkers < 1)
        nworkers = 1;
    if (nworkers > WORKERS_MAX)
        nworkers = WORKERS_MAX;
    struct worker_arg arg = {listen_fd, root_real, routes};
    pthread_t tids[WORKERS_MAX];
    int started = 0;
    for (int i = 0; i < nworkers; i++) {
        int err = pthread_create(&tids[i], NULL, worker_main, &arg);
        if (err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            break;
        }
        started++;
    }
    for (int i = 0; i < started; i++)                       /* 메인 스레드는 기다리기만 해요 */
        pthread_join(tids[i], NULL);
}
