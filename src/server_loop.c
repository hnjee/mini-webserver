#define _GNU_SOURCE /* accept4, accept(2)의 ENONET 등 Linux 오류 코드 */

/*
 * D-56, D-57: epoll 레벨 트리거 루프. 연결 목록·내부 실행 목록·마감 확인·끝난 연결 정리를 맡고,
 * 연결 하나의 상태 전이는 http_conn에 맡긴다. 한 회차 = 반환 배치 → 회차 시작 때 실행 목록 → 마감 확인 → 정리.
 */
#include "server_loop.h"
#include "http_conn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LOOP_MAX_EVENTS 64 /* epoll_wait 한 번에 받는 준비 알림 수 */
#define LOOP_ACCEPT_MAX 64 /* 회차당 accept 시도 상한(성공·재시도 오류·준비 실패 모두 1번) */

typedef struct conn {
    http_conn http;
    struct conn *prev, *next; /* 모든 연결(마감 확인) */
    struct conn *run_next;    /* 내부 실행 목록 */
    struct conn *done_next;   /* 이번 회차에 끝난 연결 */
    unsigned interest;        /* epoll에 등록한 방향: EPOLLIN 또는 EPOLLOUT */
    int waiting;              /* 마지막 차례가 WANT_READ·WANT_WRITE였다 */
    int queued;               /* 실행 목록에 있다(중복 방지) */
    int finished;             /* 끝 표시. 다시 실행하지 않는다 */
} conn;

typedef struct {
    int epfd, listen_fd;
    const char *root;
    conn *all;
    conn *run_head, *run_tail; /* 다음 회차에 실행할 연결 */
    conn *done;                /* 회차 끝에 정리할 연결 */
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

/* 끝 표시만 하고 해제는 회차 끝에 한다(C3). 같은 배치·실행 목록에 남은 포인터가 해제된 메모리를 가리키지 않는다. */
static void mark_finished(loop *l, conn *c)
{
    if (!c->finished) {
        c->finished = 1;
        c->done_next = l->done;
        l->done = c;
    }
}

static void run_conn(loop *l, conn *c)
{
    http_conn_step step = http_conn_run(&c->http, now_ns());
    unsigned want;

    if (step == HTTP_CONN_FINISHED) {
        mark_finished(l, c);
        return;
    }
    if (step == HTTP_CONN_RUNNABLE) {
        c->waiting = 0;
        if (!c->queued) {
            c->queued = 1;
            c->run_next = NULL;
            if (l->run_tail != NULL) {
                l->run_tail->run_next = c;
            } else {
                l->run_head = c;
            }
            l->run_tail = c;
        }
        return;
    }
    c->waiting = 1;
    want = step == HTTP_CONN_WANT_WRITE ? EPOLLOUT : EPOLLIN;
    if (want != c->interest) {
        struct epoll_event ev;
        ev.events = want;
        ev.data.ptr = c;
        if (epoll_ctl(l->epfd, EPOLL_CTL_MOD, c->http.fd, &ev) != 0) {
            c->http.result = HTTP_CONN_ERROR; /* 관심 방향 변경 실패는 그 연결 ERROR(D-57) */
            c->http.err = errno;
            mark_finished(l, c);
            return;
        }
        c->interest = want;
    }
}

/* 새 연결 준비. 실패하면 만든 자원까지 회수하고 그 소켓만 닫는다. */
static void add_conn(loop *l, int fd)
{
    conn *c = malloc(sizeof(*c));
    struct epoll_event ev;

    if (c == NULL) {
        close(fd);
        return;
    }
    http_conn_init(&c->http, fd, l->root, now_ns());
    c->interest = EPOLLIN;
    c->waiting = 1;
    c->queued = c->finished = 0;
    ev.events = EPOLLIN;
    ev.data.ptr = c;
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
        int fd = accept4(l->listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (accept_retryable(errno)) {
                continue;
            }
            return -1;
        }
        add_conn(l, fd);
    }
    return 0;
}

/* 기다리는 연결의 마감을 확인해 만료시키고, 남은 가장 이른 마감까지의 밀리초를 돌려준다(없으면 -1). */
static int check_deadlines(loop *l)
{
    long long now = now_ns(), earliest = LLONG_MAX;
    conn *c;

    for (c = l->all; c != NULL; c = c->next) {
        if (c->finished || !c->waiting) {
            continue;
        }
        if (http_conn_expired(&c->http, now)) {
            http_conn_expire(&c->http);
            mark_finished(l, c);
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

/* 끝난 연결 정리: 결과 기록(ERROR만 로그) → 파일 fd → 소켓 close(epoll 등록도 지워진다) → 해제. */
static void reap(loop *l)
{
    while (l->done != NULL) {
        conn *c = l->done;
        l->done = c->done_next;
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

/* 서버를 끝낼 때 남은 연결을 닫고 해제한다. */
static int fail(loop *l, const char *step, char *error, size_t error_size)
{
    int saved = errno;

    snprintf(error, error_size, "%s 실패: %s", step, strerror(saved));
    while (l->all != NULL) {
        conn *c = l->all;
        l->all = c->next;
        http_conn_release(&c->http);
        close(c->http.fd);
        free(c);
    }
    if (l->epfd >= 0) {
        close(l->epfd);
    }
    errno = saved;
    return -1;
}

int server_loop_run(int listen_fd, const char *root, char *error, size_t error_size)
{
    struct epoll_event events[LOOP_MAX_EVENTS], ev;
    loop l;
    int timeout = -1, flags;

    memset(&l, 0, sizeof(l));
    l.listen_fd = listen_fd;
    l.root = root;
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
    for (;;) {
        conn *runnable = l.run_head; /* 회차 시작 때 목록. 이번 회차에 새로 실행 가능해진 연결은 다음 회차로 간다 */
        int n, i;

        l.run_head = l.run_tail = NULL;
        n = epoll_wait(l.epfd, events, LOOP_MAX_EVENTS, runnable != NULL ? 0 : timeout);
        if (n < 0) {
            if (errno != EINTR) {
                return fail(&l, "epoll_wait", error, error_size);
            }
            n = 0;
        }
        for (i = 0; i < n; i++) { /* HUP·ERR도 깨우는 정보일 뿐이라 같은 차례 실행으로 다룬다(D-57) */
            conn *c = events[i].data.ptr;
            if (c == NULL) {
                if (accept_some(&l) != 0) {
                    return fail(&l, "accept", error, error_size);
                }
            } else if (!c->finished) {
                run_conn(&l, c);
            }
        }
        while (runnable != NULL) {
            conn *c = runnable;
            runnable = c->run_next;
            c->queued = 0;
            if (!c->finished) {
                run_conn(&l, c);
            }
        }
        timeout = check_deadlines(&l);
        reap(&l); /* 끝난 연결은 새 실행 목록에 없다: 목록에는 RUNNABLE을 돌려준(끝나지 않은) 연결만 들어간다 */
    }
}
