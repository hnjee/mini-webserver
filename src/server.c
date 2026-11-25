/*
 * 서버: docs/design/step1.md ~ step6.md
 * 리슨 소켓, epoll과 메인 루프, 연결 받기와 정리, 워커 스레드를 맡아요 (Step 5, 6).
 * 연결 하나의 읽기·보내기는 conn.c, 응답 만들기는 respond.c, 프록시는 proxy.c에 있어요.
 */
#include "server.h"

#include "log.h"
#include "proxy.h"
#include "respond.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
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

/* 리슨 소켓을 이 epoll에 등록해요. 워커 여럿이 같이 지켜봐도 손님 하나에 하나만 깨워요 (S6-01) */
static int listen_add(struct server *s)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLEXCLUSIVE;
    ev.data.ptr = &s->h_listen;
    if (epoll_ctl(s->ep, EPOLL_CTL_ADD, s->listen_fd, &ev) == 0)
        return 0;
    ev.events = EPOLLIN;                         /* 오래된 커널: EPOLLEXCLUSIVE 없이 */
    if (epoll_ctl(s->ep, EPOLL_CTL_ADD, s->listen_fd, &ev) == 0)
        return 0;
    perror("epoll_ctl ADD listen");
    return -1;
}

static void listen_del(struct server *s)
{
    if (epoll_ctl(s->ep, EPOLL_CTL_DEL, s->listen_fd, NULL) < 0)
        perror("epoll_ctl DEL listen");
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
    s->stop_fd = -1;
    s->h_stop.kind = H_STOP;
    s->h_stop.c = NULL;
    s->ep = epoll_create1(0);
    if (s->ep < 0) {
        perror("epoll_create1");
        return -1;
    }
    set_nonblock(listen_fd);
    if (listen_add(s) < 0) {
        close(s->ep);
        s->ep = -1;
        return -1;
    }
    return 0;
}

/* fd가 바닥났어요. 큐에 남은 손님 때문에 계속 깨지 않게 리슨 소켓 감시를 잠깐 쉬어요 (S7-05) */
static void pause_accept(struct server *s)
{
    if (s->accept_paused)
        return;
    fprintf(stderr, "accept: fd가 모자라요. %d초 쉬어요\n", ACCEPT_PAUSE_SEC);
    if (!s->stopping)
        listen_del(s);
    s->accept_paused = 1;
    s->accept_resume = time(NULL) + ACCEPT_PAUSE_SEC;
}

int server_on_accept(struct server *s)
{
    int count = 0;
    for (int i = 0; i < ACCEPT_BATCH; i++) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(s->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;                       /* 줄이 빔 */
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                pause_accept(s);             /* fd·메모리 바닥 (S7-05) */
                break;
            }
            perror("accept");
            break;
        }
        set_nonblock(fd);
        int one = 1;                         /* 헤더와 파일을 따로 write해도 40ms씩 묶이지 않게 (Nagle 끔) */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

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
        if (plen >= sizeof(peer) && peer.sin_family == AF_INET)
            inet_ntop(AF_INET, &peer.sin_addr, c->ip, sizeof(c->ip));

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
    access_log_conn(c);                      /* 응답 도중 끊겼거나 프록시가 끝남: 로그 (S7-06) */
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
        } else if (c->state == ST_HEADER && c->len > 0 && now - c->req_start >= HEADER_TIMEOUT_SEC) {
            conn_send_error(s, c, 408);                 /* 헤더를 30초 안에 다 못 받음 (S7-04) */
            closed++;
        } else if (now - c->last_active >= IDLE_TIMEOUT_SEC) {
            server_close_conn(s, c);
            closed++;
        }
        c = next;
    }
    if (s->accept_paused && now >= s->accept_resume) {  /* 쉬는 시간이 끝나면 다시 받아요 (S7-05) */
        s->accept_paused = 0;
        if (!s->stopping)
            listen_add(s);
    }
    return closed;
}

int server_watch_stop(struct server *s, int stop_fd)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &s->h_stop;
    if (epoll_ctl(s->ep, EPOLL_CTL_ADD, stop_fd, &ev) < 0) {
        perror("epoll_ctl ADD stop");
        return -1;
    }
    s->stop_fd = stop_fd;
    return 0;
}

int server_begin_stop(struct server *s, time_t now)
{
    if (s->stopping)
        return 0;
    s->stopping = 1;
    s->stop_at = now;
    if (s->stop_fd >= 0) {                       /* 알림은 한 번이면 돼요. 계속 깨지 않게 빼요 */
        if (epoll_ctl(s->ep, EPOLL_CTL_DEL, s->stop_fd, NULL) < 0)
            perror("epoll_ctl DEL stop");
    }
    if (!s->accept_paused)
        listen_del(s);                           /* 새 연결은 그만 받아요 */
    int closed = 0;
    struct conn *c = s->conns;
    while (c != NULL) {
        struct conn *next = c->next;
        if (c->state == ST_HEADER && c->len == 0) {   /* 다음 요청을 기다리며 쉬는 연결 */
            server_close_conn(s, c);
            closed++;
        }
        c = next;
    }
    return closed;
}

int server_should_exit(const struct server *s, time_t now)
{
    return s->stopping && (s->nconn == 0 || now - s->stop_at >= SHUTDOWN_GRACE_SEC);
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
            if (!s->stopping && !s->accept_paused)
                server_on_accept(s);                        /* 손님 받기 */
            continue;
        }
        if (h->kind == H_STOP) {
            server_begin_stop(s, time(NULL));               /* 종료 알림 (S7-02) */
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
    int stop_fd;                 /* 종료 알림 eventfd (모든 워커가 같이 지켜봐요) */
    atomic_int *alive;           /* 아직 도는 워커 수 */
};

static void *worker_main(void *p)
{
    const struct worker_arg *a = p;
    struct server s;
    if (server_init(&s, a->listen_fd, a->root) == 0) {
        s.routes = a->routes;
        if (a->stop_fd < 0 || server_watch_stop(&s, a->stop_fd) == 0) {
            for (;;) {
                /* 종료 중에는 연결이 다 닫혔는지 자주 봐요 */
                if (server_poll_once(&s, s.stopping ? 100 : TICK_MS) < 0)
                    break;
                time_t now = time(NULL);
                server_sweep_idle(&s, now);             /* 유휴 연결, 헤더 30초, 백엔드 시간 제한 */
                if (server_should_exit(&s, now))
                    break;
            }
        }
        server_free(&s);                                /* 남은 연결 닫고 해제, epoll 닫기 (S7-03) */
    }
    atomic_fetch_sub(a->alive, 1);
    return NULL;
}

/* 신호를 timeout_ms 동안 기다려요. 받았으면 신호 번호, 아니면 0 */
static int wait_signal(const sigset_t *set, int timeout_ms)
{
    struct timespec ts = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L};
    int sig = sigtimedwait(set, NULL, &ts);
    return sig > 0 ? sig : 0;
}

void server_run(int listen_fd, const char *root_real, struct routes *routes, int nworkers)
{
    if (nworkers < 1)
        nworkers = 1;
    if (nworkers > WORKERS_MAX)
        nworkers = WORKERS_MAX;

    /* 워커를 띄우기 전에 막아 두면 워커도 막힌 채로 태어나요. 그래서 종료 신호는 메인만 받아요 (S7-01) */
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, &old);

    int stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stop_fd < 0)
        perror("eventfd");
    atomic_int alive = 0;
    struct worker_arg arg = {listen_fd, root_real, routes, stop_fd, &alive};
    pthread_t tids[WORKERS_MAX];
    int started = 0;
    for (int i = 0; i < nworkers; i++) {
        atomic_fetch_add(&alive, 1);
        int err = pthread_create(&tids[i], NULL, worker_main, &arg);
        if (err != 0) {
            atomic_fetch_sub(&alive, 1);
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            break;
        }
        started++;
    }

    /* 첫 신호: 워커들에 알리고 마무리를 기다려요 */
    while (atomic_load(&alive) > 0) {
        int sig = wait_signal(&set, 200);
        if (sig == 0)
            continue;
        fprintf(stderr, "\n%s: 하던 응답을 마무리하고 꺼요 (최대 %d초, 한 번 더 누르면 바로 꺼요)\n",
                sig == SIGINT ? "SIGINT" : "SIGTERM", SHUTDOWN_GRACE_SEC);
        uint64_t one = 1;
        if (stop_fd < 0 || write(stop_fd, &one, sizeof(one)) < 0)
            _exit(1);                                   /* 알릴 방법이 없으면 바로 꺼요 */
        break;
    }
    /* 두 번째 신호: 기다리지 않고 바로 꺼요 (S7-02) */
    while (atomic_load(&alive) > 0) {
        if (wait_signal(&set, 100) != 0) {
            fprintf(stderr, "바로 꺼요\n");
            _exit(130);
        }
    }

    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);                    /* 스레드 자원 회수 (S7-03) */
    if (stop_fd >= 0)
        close(stop_fd);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}
