#define _GNU_SOURCE /* accept4, accept(2)의 ENONET 등 Linux 오류 코드 */

/*
 * D-56, D-57, D-67: epoll 레벨 트리거 루프. 연결 목록·마감 확인·끝난 연결 정리·소켓 등록을 맡고,
 * 연결 하나의 상태 전이는 http_conn에 맡긴다. 연결마다 지금 기다리는 소켓 하나만 등록하고,
 * 업로드 중에만 백엔드 읽기를 함께 등록한다.
 * 워커 확장(docs/extensions/worker-pool, 임시): 루프는 http_conn_run을 직접 부르지 않고 작업 큐에 넣는다.
 * 워커가 한 차례를 실행해 완료 큐에 넣고 eventfd로 깨운다. 넣기 전에 등록을 모두 지우므로 연결 하나는
 * 동시에 워커 하나만 처리한다. 한 회차 = 배치 모으기 → 넣기 → 완료 회수 → 마감 확인 → 정리.
 */
#include "server_loop.h"
#include "http_conn.h"

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LOOP_MAX_EVENTS 64 /* epoll_wait 한 번에 받는 준비 알림 수 */
#define LOOP_ACCEPT_MAX 64 /* 회차당 accept 시도 상한(성공·재시도 오류·준비 실패 모두 1번) */
#define LOOP_WORKERS 4     /* 워커 수(임시 기본값) */

struct conn;

/* epoll 알림이 가리키는 표: 어느 연결의 어느 소켓인가(D-67). 듣는 소켓은 NULL, eventfd는 evfd_tag다. */
typedef struct {
    struct conn *owner;
    int backend; /* 0 클라이언트, 1 백엔드 */
} tag;

static tag evfd_tag; /* owner가 NULL인 표: 완료 통지 eventfd */
static volatile sig_atomic_t stop_signal;
static volatile sig_atomic_t published_evfd = -1;

/* 핸들러와 같은 스레드에서만 eventfd를 공개·해제한다. write는 async-signal-safe다. */
void server_loop_request_stop(int signo)
{
    int saved = errno;
    int fd = published_evfd;
    uint64_t one = 1;
    if (!stop_signal) {
        stop_signal = signo;
    }
    if (fd >= 0) {
        ssize_t ignored = write(fd, &one, sizeof(one));
        (void)ignored;
    }
    errno = saved;
}


/* 실행 상태(큐 잠금 안에서만 읽고 바꾼다). 등록 대기·끝은 루프 필드(busy·finished)로 나타낸다. */
enum { RUN_IDLE, RUN_QUEUED, RUN_RUNNING, RUN_DONE };

/* 배치에서 받은 알림 하나 = 차례 하나. 백엔드 알림은 배치 직후 세대를 함께 둔다(D-67). */
typedef struct {
    int wake;
    int backend;
    unsigned gen;
} cause;

typedef struct conn {
    http_conn http;
    struct conn *prev, *next; /* 모든 연결(마감 확인·종료 해제) */
    struct conn *q_next;      /* 작업 큐·완료 큐 고리(한 번에 한 곳, 큐 잠금) */
    struct conn *touch_next;  /* 이번 배치에 알림을 받은 연결 */
    struct conn *done_next;   /* 이번 회차에 끝난 연결 */
    tag client_tag, backend_tag;
    unsigned client_events;   /* 클라이언트 소켓에 등록한 방향, 0이면 등록 안 됨 */
    unsigned backend_events;  /* 백엔드 소켓에 등록한 방향, 0이면 등록 안 됨 */
    unsigned backend_reg_gen; /* 등록한 백엔드 소켓의 세대 */
    cause causes[2];          /* 보존한 차례: 배치 순서대로 소켓마다 최대 하나 */
    int ncauses, next_cause;
    int wake;                 /* 워커에 넘길 원인. 루프가 넣기 전에 쓰고 워커가 꺼낸 뒤 읽는다 */
    http_conn_step step;      /* 마지막 차례 결과. 워커가 쓰고 루프가 회수 뒤 읽는다 */
    int sched;                /* RUN_*, 큐 잠금 */
    int busy;                 /* 루프만: 넣었고 아직 회수하지 않았다. 등록·마감 확인·정리 대상이 아니다 */
    int finished;             /* 끝 표시. 다시 실행하지 않는다 */
} conn;

typedef struct {
    pthread_mutex_t lock; /* 작업 큐·완료 큐·sched·stop */
    pthread_cond_t cond;  /* 작업 큐에 넣음·정지 */
    conn *work_head, *work_tail;
    conn *done_head, *done_tail;
    int stop;
    int evfd;
} pool;

typedef struct {
    int epfd, listen_fd;
    const char *root;
    route_table *routes;
    conn *all;
    conn *done;                    /* 회차 끝에 정리할 연결 */
    conn *touch_head, *touch_tail; /* 이번 배치에 알림을 받은 연결 */
    pool p;
    int lock_ready, cond_ready;
    pthread_t workers[LOOP_WORKERS];
    int nworkers; /* 만든 워커 수(join 대상) */
    int stopping, forced;
    long long shutdown_deadline;
    unsigned forced_count;
} loop;

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* D-07: 일시적인 accept 실패(Linux accept(2)가 재시도를 권하는 오류)인가. */
static int accept_retryable(int err)
{
    switch (err) {
    case EINTR:
    case ECONNABORTED:
    case EPROTO:
    case ENETDOWN:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
        return 1;
    default:
        return 0;
    }
}

/* ---- 워커 ---- */

/* 완료를 알린다. EAGAIN(계수 포화)은 이미 0이 아니라 루프가 깨므로 성공이다. 그 밖은 계약상 생기지 않는다. */
static void notify_loop(int evfd)
{
    uint64_t one = 1;

    for (;;) {
        if (write(evfd, &one, sizeof(one)) == (ssize_t)sizeof(one) || errno == EAGAIN) {
            return;
        }
        if (errno != EINTR) {
            fprintf(stderr, "eventfd 쓰기 실패로 중단: %s\n", strerror(errno));
            abort(); /* 정상 종료 경로가 아니다: 루프가 완료를 영영 모르게 되므로 바로 멈춘다 */
        }
    }
}

/* 작업 큐에서 연결 하나를 꺼내 http_conn_run 한 차례를 실행하고 완료 큐에 넣는다. 정지 표시를 보면 끝난다. */
static void *worker_main(void *arg)
{
    pool *p = arg;

    for (;;) {
        conn *c;

        pthread_mutex_lock(&p->lock);
        while (!p->stop && p->work_head == NULL) {
            pthread_cond_wait(&p->cond, &p->lock);
        }
        if (p->stop) { /* 작업 큐에 남은 연결은 실행하지 않는다 */
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }
        c = p->work_head;
        p->work_head = c->q_next;
        if (p->work_head == NULL) {
            p->work_tail = NULL;
        }
        assert(c->sched == RUN_QUEUED); /* 연결당 워커 하나 */
        c->sched = RUN_RUNNING;
        pthread_mutex_unlock(&p->lock);

        c->step = http_conn_run(&c->http, c->wake, now_ns()); /* 차례를 시작할 때의 현재 시각 */

        pthread_mutex_lock(&p->lock);
        assert(c->sched == RUN_RUNNING);
        c->sched = RUN_DONE;
        c->q_next = NULL;
        if (p->done_tail != NULL) {
            p->done_tail->q_next = c;
        } else {
            p->done_head = c;
        }
        p->done_tail = c;
        pthread_mutex_unlock(&p->lock);
        notify_loop(p->evfd); /* 넣은 뒤에 쓴다: 깨우기 손실 없음 */
    }
}

/* 정지 표시 → 모두 깨움 → 잠금 밖에서 만든 워커만 join. 실행 중 차례는 마치고 완료 큐에 넣은 뒤 끝난다. */
static void stop_workers(loop *l)
{
    int i;

    if (l->nworkers == 0) {
        return;
    }
    pthread_mutex_lock(&l->p.lock);
    l->p.stop = 1;
    pthread_cond_broadcast(&l->p.cond);
    pthread_mutex_unlock(&l->p.lock);
    for (i = 0; i < l->nworkers; i++) {
        pthread_join(l->workers[i], NULL);
    }
    l->nworkers = 0;
}

/* ---- 루프 ---- */

/* 끝 표시만 하고 해제는 회차 끝에 한다(C3). 같은 배치에 남은 포인터가 해제된 메모리를 가리키지 않는다. */
static void mark_finished(loop *l, conn *c)
{
    if (!c->finished) {
        c->finished = 1;
        c->ncauses = c->next_cause = 0; /* 보존 차례는 버린다 */
        c->done_next = l->done;
        l->done = c;
    }
}

/* 등록 변경 실패는 그 연결 ERROR(D-57, D-67). errno를 그대로 기록한다. */
static void conn_error(loop *l, conn *c)
{
    c->http.result = HTTP_CONN_ERROR;
    c->http.err = errno;
    mark_finished(l, c);
}

/* 한 소켓의 등록을 want로 맞춘다: 0이면 DEL, 처음이면 ADD, 그 밖은 MOD. *events는 등록한 방향이다. */
static int set_interest(loop *l, int fd, tag *t, unsigned *events, unsigned want)
{
    struct epoll_event ev;
    int op;

    if (*events == want) {
        return 0;
    }
    op = want == 0 ? EPOLL_CTL_DEL : *events == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    ev.events = want;
    ev.data.ptr = t;
    if (epoll_ctl(l->epfd, op, fd, &ev) != 0) {
        return -1;
    }
    *events = want;
    return 0;
}

/* 등록해 둔 백엔드가 닫혔으면(세대가 바뀜) close가 등록도 지웠다. DEL 없이 0으로 둔다. */
static void fix_backend_gen(conn *c)
{
    if (c->backend_events != 0 && c->backend_reg_gen != c->http.be_gen) {
        c->backend_events = 0;
    }
}

/* 기다리는 차례 결과에 맞춰 두 소켓의 등록을 바꾼다(D-67). 실패하면 -1(errno). */
static int apply_interest(loop *l, conn *c, http_conn_step step)
{
    unsigned want_client = step == HTTP_CONN_WANT_READ ? EPOLLIN : step == HTTP_CONN_WANT_WRITE ? EPOLLOUT : 0;
    unsigned want_backend = 0;

    if (step == HTTP_CONN_WANT_BACKEND_READ || (step == HTTP_CONN_WANT_READ && c->http.uploading)) {
        want_backend = EPOLLIN;
    } else if (step == HTTP_CONN_WANT_BACKEND_WRITE) {
        want_backend = EPOLLOUT | (c->http.uploading ? EPOLLIN : 0);
    }
    fix_backend_gen(c); /* 같은 fd 번호의 새 소켓은 DEL 없이 ADD한다 */
    if (set_interest(l, c->http.fd, &c->client_tag, &c->client_events, want_client) != 0) {
        return -1;
    }
    if (c->http.be_fd >= 0 &&
        set_interest(l, c->http.be_fd, &c->backend_tag, &c->backend_events, want_backend) != 0) {
        return -1;
    }
    c->backend_reg_gen = c->http.be_gen;
    return 0;
}

/* 넣기·만료 전에 유효한 등록만 모두 지운다(세대 보정 먼저). 실패하면 -1(errno). */
static int unregister(loop *l, conn *c)
{
    fix_backend_gen(c);
    if (set_interest(l, c->http.fd, &c->client_tag, &c->client_events, 0) != 0) {
        return -1;
    }
    if (c->backend_events != 0 && set_interest(l, c->http.be_fd, &c->backend_tag, &c->backend_events, 0) != 0) {
        return -1;
    }
    return 0;
}

static void begin_shutdown(loop *l);

/* 루프 스레드의 신호 마스크 변경. 인자가 고정이라 실패는 계약상 없다: 순서 보장이 깨지므로 바로 멈춘다. */
static void set_stop_mask(int how, const sigset_t *set, sigset_t *old)
{
    int rc = pthread_sigmask(how, set, old);
    if (rc != 0) {
        fprintf(stderr, "신호 마스크 변경 실패로 중단: %s\n", strerror(rc));
        abort();
    }
}

/*
 * 작업 큐에 넣는다(admission). 등록이 없는(바쁘지 않은) 연결만 넣는다.
 * 선후 기준(D-69 임시): 두 신호를 막고 최신 정지 표시를 본 뒤 큐 삽입까지 마치고 원래 마스크로 되돌린다.
 * 막기 전에 처리된 신호면 정리를 먼저 시작해 이 연결에도 정지를 적용한다(요청 사이면 넣지 않고 끝,
 * 이미 시작한 요청이면 그 차례를 넣는다). 막은 동안 온 신호는 보류됐다가 삽입 뒤 복원에서 전달된다.
 */
static void submit(loop *l, conn *c, int wake)
{
    sigset_t stops, old;

    sigemptyset(&stops);
    sigaddset(&stops, SIGINT);
    sigaddset(&stops, SIGTERM);
    for (;;) {
        set_stop_mask(SIG_BLOCK, &stops, &old);
        if (!stop_signal || l->stopping) {
            break;
        }
        set_stop_mask(SIG_SETMASK, &old, NULL);
        begin_shutdown(l);
        if (c->finished) {
            return;
        }
    }
    assert(!c->finished && !c->busy && c->client_events == 0 && c->backend_events == 0);
    c->busy = 1;
    c->wake = wake;
    pthread_mutex_lock(&l->p.lock);
    assert(c->sched == RUN_IDLE); /* 연결당 워커 하나 */
    c->sched = RUN_QUEUED;
    c->q_next = NULL;
    if (l->p.work_tail != NULL) {
        l->p.work_tail->q_next = c;
    } else {
        l->p.work_head = c;
    }
    l->p.work_tail = c;
    pthread_cond_signal(&l->p.cond);
    pthread_mutex_unlock(&l->p.lock);
    set_stop_mask(SIG_SETMASK, &old, NULL); /* 보류 신호는 삽입 뒤 여기서 전달된다 */
}

/* 보존 차례 중 다음 것을 넣는다. 백엔드 차례는 넣기 직전에 세대를 다시 비교해 다르면 버린다. 넣었으면 1. */
static int submit_next_cause(loop *l, conn *c)
{
    while (c->next_cause < c->ncauses) {
        const cause *k = &c->causes[c->next_cause++];
        if (k->backend && (k->gen != c->http.be_gen || c->http.be_fd < 0)) {
            continue; /* 앞 차례가 백엔드를 닫거나 바꿨다: 옛 소켓 알림 */
        }
        submit(l, c, k->wake);
        return 1;
    }
    c->ncauses = c->next_cause = 0;
    return 0;
}

/* 보존 차례가 없을 때 마지막 차례 결과로만 다음을 정한다: 내부 실행이나 다시 등록(그 뒤 대기). */
static void settle(loop *l, conn *c)
{
    if (c->step == HTTP_CONN_RUNNABLE) {
        submit(l, c, HTTP_CONN_WAKE_INTERNAL);
    } else if (apply_interest(l, c, c->step) != 0) {
        conn_error(l, c);
    }
}

/* 완료 큐에서 꺼낸 연결: FINISHED면 끝, 보존 차례가 남았으면 다음 차례, 아니면 마지막 결과대로. */
/* 새 요청을 넣거나 받기 직전마다 최신 신호를 반영한다: 회차 맨 앞 확인 뒤 온 신호도 admission보다 먼저 본다. */
static void observe_stop(loop *l)
{
    if (stop_signal && !l->stopping) {
        begin_shutdown(l);
    }
}

static void collect(loop *l, conn *c)
{
    c->busy = 0;
    observe_stop(l); /* 정리 시작이 이 연결에도 정지를 적용했을 수 있다 */
    if (c->finished) {
        return;
    }
    if (l->forced) {
        c->http.result = HTTP_CONN_ERROR;
        c->http.err = ECANCELED;
        l->forced_count++;
        mark_finished(l, c);
    } else if (c->step == HTTP_CONN_FINISHED) {
        mark_finished(l, c);
    } else if (l->stopping && http_conn_stop(&c->http) == HTTP_CONN_FINISHED) {
        mark_finished(l, c);
    } else if (!submit_next_cause(l, c)) {
        settle(l, c);
    }
}

/* 배치의 알림 하나를 연결의 보존 차례로 더한다(원인을 합치지 않는다). */
static void add_cause(loop *l, conn *c, int wake, int backend, unsigned gen)
{
    if (c->ncauses == 0) {
        c->touch_next = NULL;
        if (l->touch_tail != NULL) {
            l->touch_tail->touch_next = c;
        } else {
            l->touch_head = c;
        }
        l->touch_tail = c;
    }
    assert(c->ncauses < 2); /* 소켓마다 알림은 배치에 한 번이다 */
    c->causes[c->ncauses].wake = wake;
    c->causes[c->ncauses].backend = backend;
    c->causes[c->ncauses].gen = gen;
    c->ncauses++;
}

/* 새 연결 준비. 실패하면 만든 자원까지 회수하고 그 소켓만 닫는다. */
static void add_conn(loop *l, int fd, const struct sockaddr_storage *addr)
{
    conn *c = malloc(sizeof(*c));
    struct epoll_event ev;
    char peer[64] = "-";

    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip)))
            snprintf(peer, sizeof(peer), "%s:%u", ip, (unsigned)ntohs(in->sin_port));
    }

    if (c == NULL) {
        close(fd);
        return;
    }
    http_conn_init(&c->http, fd, l->root, l->routes, peer, now_ns());
    c->client_tag.owner = c->backend_tag.owner = c;
    c->client_tag.backend = 0;
    c->backend_tag.backend = 1;
    c->client_events = EPOLLIN;
    c->backend_events = 0;
    c->backend_reg_gen = 0;
    c->ncauses = c->next_cause = 0;
    c->step = HTTP_CONN_WANT_READ;
    c->sched = RUN_IDLE;
    c->busy = c->finished = 0;
    ev.events = EPOLLIN;
    ev.data.ptr = &c->client_tag;
    if (epoll_ctl(l->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        free(c);
        close(fd);
        return;
    }
    c->prev = NULL;
    c->next = l->all;
    if (l->all != NULL) {
        l->all->prev = c;
    }
    l->all = c;
}

/* 회차당 LOOP_ACCEPT_MAX번까지 받는다. 서버를 끝내야 하는 오류면 -1(errno 보존). */
static int accept_some(loop *l)
{
    int i;

    for (i = 0; i < LOOP_ACCEPT_MAX; i++) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        int fd = accept4(l->listen_fd, (struct sockaddr *)&addr, &len, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (accept_retryable(errno)) {
                continue;
            }
            return -1;
        }
        add_conn(l, fd, &addr);
    }
    return 0;
}

/* eventfd를 비운다(EINTR 다시, EAGAIN 무시). 그 밖은 서버를 끝내는 실패로 -1. */
static int drain_eventfd(loop *l)
{
    uint64_t v;

    for (;;) {
        if (read(l->p.evfd, &v, sizeof(v)) == (ssize_t)sizeof(v) || errno == EAGAIN) {
            return 0;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

/* 완료 큐를 모두 꺼내 회수한다. 실행 상태는 잠금 안에서 바꾸고, HTTP·등록 처리는 잠금 밖에서 한다. */
static void collect_done(loop *l)
{
    conn *c, *list;

    pthread_mutex_lock(&l->p.lock);
    list = l->p.done_head;
    l->p.done_head = l->p.done_tail = NULL;
    for (c = list; c != NULL; c = c->q_next) {
        assert(c->sched == RUN_DONE);
        c->sched = RUN_IDLE;
    }
    pthread_mutex_unlock(&l->p.lock);
    while (list != NULL) {
        c = list;
        list = c->q_next; /* collect가 다시 넣으면 q_next가 바뀐다 */
        collect(l, c);
    }
}

/* 대기 중인(바쁘지 않은) 연결의 마감을 확인해 만료시키고, 남은 가장 이른 마감까지의 밀리초를 돌려준다(없으면 -1). */
static int check_deadlines(loop *l)
{
    long long now = now_ns(), earliest = LLONG_MAX;
    conn *c;

    for (c = l->all; c != NULL; c = c->next) {
        if (c->finished || c->busy) {
            continue;
        }
        if (http_conn_expired(&c->http, now)) {
            if (unregister(l, c) != 0) { /* 만료 전에 지운다: 커밋 전 백엔드 만료는 백엔드를 닫는다 */
                conn_error(l, c);
            } else if ((c->step = http_conn_expire(&c->http)) == HTTP_CONN_RUNNABLE) {
                submit(l, c, HTTP_CONN_WAKE_INTERNAL); /* 커밋 전 백엔드 만료: 504 순서(D-67) */
            } else {
                mark_finished(l, c);
            }
        } else if (c->http.deadline < earliest) {
            earliest = c->http.deadline;
        }
    }
    if (earliest == LLONG_MAX) {
        return -1;
    }
    earliest = (earliest - now + 999999) / 1000000; /* 올림: 마감 전에 깨어 헛돌지 않게 한다 */
    return earliest > INT_MAX ? INT_MAX : (int)earliest;
}

/* 종료 표시를 루프에서 적용한다. 워커 실행 중인 연결은 완료 회수 뒤에만 만진다. */
static void begin_shutdown(loop *l)
{
    conn *c;
    int count = 0;
    l->stopping = 1;
    l->shutdown_deadline = now_ns() + 10000000000LL;
    close(l->listen_fd);
    l->listen_fd = -1;
    for (c = l->all; c != NULL; c = c->next) {
        count++;
        if (!c->busy && !c->finished && http_conn_stop(&c->http) == HTTP_CONN_FINISHED) {
            mark_finished(l, c);
        }
    }
    printf("종료 신호 SIG%s: 새 연결 중단, 연결 %d개 마무리 (최대 10초)\n",
           stop_signal == SIGINT ? "INT" : "TERM", count);
}

static void force_shutdown(loop *l)
{
    conn *c;
    l->forced = 1;
    for (c = l->all; c != NULL; c = c->next) {
        if (!c->busy && !c->finished) {
            c->http.result = HTTP_CONN_ERROR;
            c->http.err = ECANCELED;
            l->forced_count++;
            mark_finished(l, c);
        }
    }
}

/* 끝난 연결 정리: 결과 기록(ERROR만 로그) → 파일 fd·백엔드 소켓 → 소켓 close(epoll 등록도 지워진다) → 해제. */
static void reap(loop *l)
{
    while (l->done != NULL) {
        conn *c = l->done;
        l->done = c->done_next;
        assert(!c->busy);
        if (c->http.result == HTTP_CONN_ERROR) {
            fprintf(stderr, "연결 오류로 닫음: %s\n", strerror(c->http.err));
        }
        http_conn_release(&c->http);
        close(c->http.fd);
        if (c->prev != NULL) {
            c->prev->next = c->next;
        } else {
            l->all = c->next;
        }
        if (c->next != NULL) {
            c->next->prev = c->prev;
        }
        free(c);
    }
}

/* 서버를 끝낼 때: 워커를 멈춘 뒤(join) 남은 연결을 닫고 해제하고, eventfd·잠금·epoll을 정리한다. */
static void cleanup(loop *l)
{
    stop_workers(l);
    published_evfd = -1;
    while (l->all != NULL) {
        conn *c = l->all;
        l->all = c->next;
        http_conn_release(&c->http);
        close(c->http.fd);
        free(c);
    }
    if (l->listen_fd >= 0) {
        close(l->listen_fd);
        l->listen_fd = -1;
    }
    if (l->p.evfd >= 0) {
        close(l->p.evfd);
    }
    if (l->cond_ready) {
        pthread_cond_destroy(&l->p.cond);
    }
    if (l->lock_ready) {
        pthread_mutex_destroy(&l->p.lock);
    }
    if (l->epfd >= 0) {
        close(l->epfd);
    }
}

static int fail(loop *l, const char *step, char *error, size_t error_size)
{
    int saved = errno;
    snprintf(error, error_size, "%s 실패: %s", step, strerror(saved));
    cleanup(l);
    errno = saved;
    return -1;
}

/* 시작 준비: eventfd·잠금·조건 변수·워커. 실패면 errno를 두고 -1과 단계 이름. */
static int start_pool(loop *l, const char **step)
{
    struct epoll_event ev;
    sigset_t blocked, old;
    int rc, i;

    *step = "eventfd";
    l->p.evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (l->p.evfd < 0) {
        return -1;
    }
    ev.events = EPOLLIN;
    ev.data.ptr = &evfd_tag;
    *step = "epoll_ctl(eventfd)";
    if (epoll_ctl(l->epfd, EPOLL_CTL_ADD, l->p.evfd, &ev) != 0) {
        return -1;
    }
    *step = "큐 잠금 준비";
    if ((rc = pthread_mutex_init(&l->p.lock, NULL)) != 0) {
        errno = rc;
        return -1;
    }
    l->lock_ready = 1;
    if ((rc = pthread_cond_init(&l->p.cond, NULL)) != 0) {
        errno = rc;
        return -1;
    }
    l->cond_ready = 1;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    *step = "워커 신호 마스크 설정";
    if ((rc = pthread_sigmask(SIG_BLOCK, &blocked, &old)) != 0) {
        errno = rc;
        return -1;
    }
    *step = "워커 스레드 만들기";
    for (i = 0; i < LOOP_WORKERS; i++) {
        if ((rc = pthread_create(&l->workers[i], NULL, worker_main, &l->p)) != 0) {
            (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
            errno = rc;
            return -1; /* 만든 워커는 fail이 멈추고 join한다 */
        }
        l->nworkers++;
    }
    if ((rc = pthread_sigmask(SIG_SETMASK, &old, NULL)) != 0) {
        errno = rc;
        return -1;
    }
    published_evfd = l->p.evfd;
    return 0;
}

int server_loop_run(int listen_fd, const char *root, route_table *routes, char *error, size_t error_size)
{
    struct epoll_event events[LOOP_MAX_EVENTS], ev;
    unsigned gens[LOOP_MAX_EVENTS]; /* 배치를 받은 직후 복사한 백엔드 세대(D-67) */
    loop l;
    int timeout = -1, flags;
    const char *step;

    memset(&l, 0, sizeof(l));
    l.listen_fd = listen_fd;
    l.root = root;
    l.routes = routes;
    l.p.evfd = -1;
    l.epfd = epoll_create1(0);
    if (l.epfd < 0) {
        return fail(&l, "epoll_create1", error, error_size);
    }
    flags = fcntl(listen_fd, F_GETFL);
    if (flags < 0 || fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return fail(&l, "듣는 소켓 논블로킹 설정", error, error_size);
    }
    ev.events = EPOLLIN;
    ev.data.ptr = NULL; /* NULL = 듣는 소켓 */
    if (epoll_ctl(l.epfd, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
        return fail(&l, "epoll_ctl(듣는 소켓)", error, error_size);
    }
    if (start_pool(&l, &step) != 0) {
        return fail(&l, step, error, error_size);
    }
    for (;;) {
        int n, i, woke = 0;
        conn *c;
        long long remaining;

        if (stop_signal && !l.stopping) {
            begin_shutdown(&l);
        }
        if (l.stopping && !l.forced && now_ns() >= l.shutdown_deadline) {
            force_shutdown(&l);
        }
        reap(&l);
        if (l.stopping && l.all == NULL) {
            if (l.forced_count != 0) {
                fprintf(stderr, "강제 종료 연결 %u개\n", l.forced_count);
            }
            cleanup(&l);
            puts("서버 종료");
            return 0;
        }
        if (l.stopping && !l.forced) {
            remaining = (l.shutdown_deadline - now_ns() + 999999) / 1000000;
            if (remaining < 0) remaining = 0;
            if (timeout < 0 || timeout > remaining) timeout = (int)remaining;
        }
        n = epoll_wait(l.epfd, events, LOOP_MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno != EINTR) {
                return fail(&l, "epoll_wait", error, error_size);
            }
            n = 0;
        }
        if (stop_signal && !l.stopping) {
            begin_shutdown(&l); /* 잠든 동안 온 신호: stale 듣는 소켓 알림을 처리하지 않는다 */
        }
        if (l.stopping && !l.forced && now_ns() >= l.shutdown_deadline) {
            force_shutdown(&l);
        }
        for (i = 0; i < n; i++) { /* 아무것도 넣기 전에 백엔드 알림의 세대를 복사한다 */
            tag *t = events[i].data.ptr;
            gens[i] = t != NULL && t->owner != NULL ? t->owner->http.be_gen : 0;
        }
        l.touch_head = l.touch_tail = NULL;
        for (i = 0; i < n; i++) { /* 연결별로 알림마다 차례를 배치 순서대로 보존한다. HUP·ERR도 깨우는 정보다(D-57) */
            tag *t = events[i].data.ptr;
            if (t == NULL) {
                observe_stop(&l);
                if (!l.stopping && accept_some(&l) != 0) {
                    return fail(&l, "accept", error, error_size);
                }
                continue;
            }
            if (t == &evfd_tag) {
                woke = 1;
                continue;
            }
            if (l.forced) continue;
            c = t->owner;
            assert(!c->busy); /* 바쁜 연결은 등록이 없다 */
            if (c->finished) {
                continue;
            }
            if (!t->backend) {
                add_cause(&l, c, HTTP_CONN_WAKE_CLIENT, 0, 0);
            } else if (c->http.be_gen == gens[i] && c->http.be_fd >= 0) { /* 세대가 바뀌었으면 옛 소켓 알림이다 */
                unsigned e = events[i].events;
                add_cause(&l, c,
                          (e & EPOLLIN ? HTTP_CONN_WAKE_BACKEND_IN : 0) | (e & EPOLLOUT ? HTTP_CONN_WAKE_BACKEND_OUT : 0) |
                              (e & (EPOLLERR | EPOLLHUP) ? HTTP_CONN_WAKE_BACKEND_ERR : 0),
                          1, gens[i]);
            }
        }
        for (c = l.touch_head; c != NULL; c = c->touch_next) { /* 유효 등록을 지우고 첫 차례를 넣는다 */
            c->next_cause = 0;
            observe_stop(&l); /* 정리 시작이 요청 사이 연결을 이미 끝냈으면 넣지 않는다 */
            if (c->finished) {
                continue;
            }
            if (unregister(&l, c) != 0) {
                conn_error(&l, c);
            } else if (!submit_next_cause(&l, c)) {
                settle(&l, c);
            }
        }
        if (woke && drain_eventfd(&l) != 0) { /* 읽은 뒤에 완료 큐를 비운다: 깨우기 손실 없음 */
            return fail(&l, "eventfd 읽기", error, error_size);
        }
        collect_done(&l); /* eventfd 결과와 관계없이 매 회차 비운다 */
        timeout = l.forced ? -1 : check_deadlines(&l); /* 강제 이후에는 새 작업을 넣지 않는다 */
        reap(&l);
    }
}
