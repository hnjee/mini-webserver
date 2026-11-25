/*
 * 안정성 테스트: docs/tests/step7.md UT-01 ~ UT-21
 * 앞쪽(UT-01 ~ UT-16)은 test_event.c처럼 알림이 왔다고 치고 서버 함수를 직접 부른다.
 * 시간은 기다리지 않고 server_sweep_idle(s, 지금 + N)으로 건너뛴다.
 * 뒤쪽(UT-17 ~ UT-20)은 자식 프로세스에서 server_run()을 돌리고 진짜 신호를 보낸다.
 */
/* realpath()는 POSIX의 XSI 부분이라 _XOPEN_SOURCE를 켜야 선언된다 */
#define _XOPEN_SOURCE 700

#include "log.h"
#include "server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_run = 0;
static int g_fail = 0;

#define CHECK(id, cond)                                                   \
    do {                                                                  \
        g_run++;                                                          \
        if (cond) {                                                       \
            printf("  ok    %s\n", id);                                   \
        } else {                                                          \
            g_fail++;                                                     \
            printf("  FAIL  %s  (%s:%d) %s\n", id, __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

/* ---- 테스트용 루트 ---- */

#define BIG_SIZE (1024 * 1024)

static char g_base[] = "/tmp/ws_stable_XXXXXX";
static char g_root[PATH_MAX];
static char *g_big;               /* big.bin 내용 */

static const char INDEX_HTML[] = "<h1>hi</h1>\n";
static const char STYLE_CSS[] = "body { color: red; }\n";

static int write_file(const char *dir, const char *name, const char *data, size_t len)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f == NULL)
        return -1;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len ? 0 : -1;
}

/* <base>/www 에 index.html, style.css, big.bin(1MB 바이트 패턴)을 만든다 */
static int make_fixture(void)
{
    char www[PATH_MAX];
    if (mkdtemp(g_base) == NULL)
        return 0;
    snprintf(www, sizeof(www), "%s/www", g_base);
    if (mkdir(www, 0755) < 0)
        return 0;
    g_big = malloc(BIG_SIZE);
    if (g_big == NULL)
        return 0;
    for (size_t i = 0; i < BIG_SIZE; i++)
        g_big[i] = (char)((i * 31 + 7) % 251);
    return write_file(www, "index.html", INDEX_HTML, sizeof(INDEX_HTML) - 1) == 0 &&
           write_file(www, "style.css", STYLE_CSS, sizeof(STYLE_CSS) - 1) == 0 &&
           write_file(www, "big.bin", g_big, BIG_SIZE) == 0 &&
           realpath(www, g_root) != NULL;
}

static void remove_fixture(void)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0)
        printf("  (임시 폴더 %s를 지우지 못함)\n", g_base);
    free(g_big);
}

/* ---- 도우미 ---- */

static int count_open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL)
        return -1;
    int n = 0;
    while (readdir(d) != NULL)
        n++;
    closedir(d);
    return n;
}

static int g_port;

/* 리슨 소켓을 열고 server_init까지. 실패하면 -1 */
static int setup(struct server *s, int *lfd)
{
    *lfd = server_listen(0);
    if (*lfd < 0)
        return -1;
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    getsockname(*lfd, (struct sockaddr *)&a, &len);
    g_port = ntohs(a.sin_port);
    memset(s, 0, sizeof(*s));
    return server_init(s, *lfd, g_root);
}

static void teardown(struct server *s, int lfd)
{
    server_free(s);
    close(lfd);
}

/* 접속만 하고 논블로킹으로 바꾼 클라이언트 소켓. rcvbuf > 0이면 받기 버퍼를 그만큼으로 줄인다 */
static int client_connect(int rcvbuf)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (rcvbuf > 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)g_port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* 클라이언트 fd와 짝인 서버 쪽 conn을 찾는다 (서버 쪽 상대 포트 == 클라이언트 자기 포트) */
static struct conn *find_conn(struct server *s, int client_fd)
{
    struct sockaddr_in me, peer;
    socklen_t l1 = sizeof(me);
    getsockname(client_fd, (struct sockaddr *)&me, &l1);
    for (struct conn *c = s->conns; c != NULL; c = c->next) {
        socklen_t l2 = sizeof(peer);
        if (getpeername(c->fd, (struct sockaddr *)&peer, &l2) == 0 && peer.sin_port == me.sin_port)
            return c;
    }
    return NULL;
}

/* 접속하고 server_on_accept()까지 해서 짝 conn을 돌려준다 */
static struct conn *connect_and_accept(struct server *s, int *cfd, int rcvbuf)
{
    *cfd = client_connect(rcvbuf);
    if (*cfd < 0)
        return NULL;
    struct pollfd p = {s->listen_fd, POLLIN, 0};
    poll(&p, 1, 1000);
    server_on_accept(s);
    return find_conn(s, *cfd);
}

static int conn_alive(struct server *s, struct conn *c)
{
    for (struct conn *x = s->conns; x != NULL; x = x->next)
        if (x == c)
            return 1;
    return 0;
}

static void send_str(int fd, const char *str)
{
    size_t len = strlen(str), sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, str + sent, len - sent);
        if (n > 0)
            sent += (size_t)n;
        else if (n < 0 && errno != EAGAIN)
            return;
    }
}

/* 서버 쪽 소켓에 읽을 것(또는 끊김)이 올 때까지 최대 1초 기다린 뒤 conn_on_readable() */
static void readable(struct server *s, struct conn *c)
{
    struct pollfd p = {c->fd, POLLIN, 0};
    poll(&p, 1, 1000);
    conn_on_readable(s, c);
}

/* 클라이언트가 받은 것. eof는 서버가 연결을 닫았으면 1 */
struct got {
    char *buf;
    size_t len;
    int eof;
};

/* wait_ms 동안 새 데이터가 안 올 때까지 읽는다 */
static void recv_all(int fd, struct got *g, int wait_ms)
{
    size_t cap = BIG_SIZE + 65536;
    g->buf = malloc(cap + 1);
    g->len = 0;
    g->eof = 0;
    if (g->buf == NULL)
        return;
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, wait_ms) <= 0)
            break;
        ssize_t n = read(fd, g->buf + g->len, cap - g->len);
        if (n == 0) {
            g->eof = 1;
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN)
                continue;
            break;
        }
        g->len += (size_t)n;
        if (g->len == cap)
            break;
    }
    g->buf[g->len] = '\0';
}

/* 응답 하나의 본문이 data와 같은지 (응답이 하나일 때) */
static int body_is(const struct got *g, const char *data, size_t len)
{
    const char *h = strstr(g->buf, "\r\n\r\n");
    if (h == NULL)
        return 0;
    size_t off = (size_t)(h + 4 - g->buf);
    return g->len - off == len && memcmp(g->buf + off, data, len) == 0;
}

static int has(const struct got *g, const char *s)
{
    return strstr(g->buf, s) != NULL;
}

#define GET(path) "GET " path " HTTP/1.1\r\nHost: x\r\n\r\n"


static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ---- 종료 (S7-02) ---- */

static void ut01_to_ut04_stop(void)
{
    struct server s;
    int lfd;
    if (setup(&s, &lfd) < 0) {
        CHECK("UT-01 준비", 0);
        return;
    }
    int idle_fd, part_fd, big_fd;
    struct conn *idle = connect_and_accept(&s, &idle_fd, 0);        /* 아무것도 안 보낸 연결 */
    struct conn *part = connect_and_accept(&s, &part_fd, 0);        /* 헤더 일부만 보낸 연결 */
    struct conn *big = connect_and_accept(&s, &big_fd, 4096);       /* 큰 파일 받는 중인 연결 */
    send_str(part_fd, "GET /index.html HTTP/1.1\r\nHo");
    readable(&s, part);
    int small = 4096;
    if (big != NULL)
        setsockopt(big->fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));   /* 서버 보내기 버퍼도 줄여 1MB가 한 번에 안 나가게 */
    send_str(big_fd, GET("/big.bin"));
    readable(&s, big);
    CHECK("UT-01 준비: 연결 3개, big은 보내는 중", s.nconn == 3 && big->state == ST_WRITE);

    int closed = server_begin_stop(&s, time(NULL));
    CHECK("UT-01 쉬는 연결만 닫음 (1개)", closed == 1 && !conn_alive(&s, idle) && s.nconn == 2);
    struct got g;
    recv_all(idle_fd, &g, 200);
    CHECK("UT-01 쉬던 클라이언트는 응답 없이 EOF", g.len == 0 && g.eof);
    free(g.buf);

    /* UT-02: 새 손님은 안 받음 */
    int new_fd = client_connect(0);
    server_poll_once(&s, 100);
    CHECK("UT-02 종료 중에는 새 연결을 안 받음", s.nconn == 2);

    /* UT-03: 헤더를 마저 보내면 응답하고 닫음 */
    send_str(part_fd, "st: x\r\n\r\n");
    readable(&s, part);
    recv_all(part_fd, &g, 200);
    CHECK("UT-03 하던 요청은 200", strncmp(g.buf, "HTTP/1.1 200 ", 13) == 0);
    CHECK("UT-03 Connection: close, 그 뒤 닫음", has(&g, "Connection: close") && g.eof);
    free(g.buf);

    /* UT-04: 보내던 1MB는 끝까지 */
    size_t total = 0;
    int eof = 0;
    char *rb = malloc(BIG_SIZE + 4096);
    for (int i = 0; i < 2000 && !eof && rb != NULL; i++) {
        if (conn_alive(&s, big))
            conn_on_writable(&s, big);
        ssize_t n = read(big_fd, rb + total, BIG_SIZE + 4096 - total);
        if (n > 0)
            total += (size_t)n;
        else if (n == 0)
            eof = 1;
        else
            sleep_ms(1);
    }
    const char *h = NULL;
    for (size_t i = 0; rb != NULL && i + 4 <= total && h == NULL; i++)
        if (memcmp(rb + i, "\r\n\r\n", 4) == 0)
            h = rb + i;
    size_t off = h ? (size_t)(h + 4 - rb) : 0;
    CHECK("UT-04 보내던 1MB를 끝까지 받음", h != NULL && total - off == BIG_SIZE && memcmp(rb + off, g_big, BIG_SIZE) == 0);
    CHECK("UT-04 다 보낸 뒤 닫음", eof && s.nconn == 0);
    free(rb);
    close(idle_fd);
    close(part_fd);
    close(big_fd);
    close(new_fd);
    teardown(&s, lfd);
}

static void ut05_should_exit(void)
{
    struct server s;
    int lfd;
    if (setup(&s, &lfd) < 0) {
        CHECK("UT-05 준비", 0);
        return;
    }
    time_t now = time(NULL);
    CHECK("UT-05 종료 전에는 0", server_should_exit(&s, now) == 0);
    int cfd;
    struct conn *c = connect_and_accept(&s, &cfd, 0);
    send_str(cfd, "GET / HTTP/1.1\r\n");               /* 하던 요청이 있는 연결 */
    readable(&s, c);
    server_begin_stop(&s, now);
    CHECK("UT-05 연결이 남아 있고 4초면 0", server_should_exit(&s, now + SHUTDOWN_GRACE_SEC - 1) == 0);
    CHECK("UT-05 5초가 지나면 1", server_should_exit(&s, now + SHUTDOWN_GRACE_SEC) == 1);
    server_close_conn(&s, c);
    CHECK("UT-05 연결이 다 닫히면 바로 1", server_should_exit(&s, now) == 1);
    close(cfd);
    teardown(&s, lfd);
}

static void ut06_stop_fd(void)
{
    struct server s;
    int lfd;
    if (setup(&s, &lfd) < 0) {
        CHECK("UT-06 준비", 0);
        return;
    }
    int efd = eventfd(0, EFD_NONBLOCK);
    CHECK("UT-06 eventfd 등록", efd >= 0 && server_watch_stop(&s, efd) == 0);
    server_poll_once(&s, 50);
    CHECK("UT-06 알림 전에는 종료 아님", s.stopping == 0);
    uint64_t one = 1;
    CHECK("UT-06 알림 쓰기", write(efd, &one, sizeof(one)) == sizeof(one));
    server_poll_once(&s, 500);
    CHECK("UT-06 알림이 오면 종료 시작", s.stopping == 1);
    CHECK("UT-06 한 번 받은 뒤에는 다시 깨지 않음", server_poll_once(&s, 50) == 0);
    teardown(&s, lfd);
    close(efd);
}

/* ---- 헤더 30초 (S7-04) ---- */

static void ut07_to_ut09_header_timeout(void)
{
    struct server s;
    int lfd;
    if (setup(&s, &lfd) < 0) {
        CHECK("UT-07 준비", 0);
        return;
    }
    int cfd;
    struct conn *c = connect_and_accept(&s, &cfd, 0);
    time_t t0 = time(NULL);
    send_str(cfd, "GET /index.html HTTP/1.1\r\nHo");
    readable(&s, c);
    c->last_active = t0 + 28;                            /* 28초까지 1바이트씩 왔다고 침 */
    server_sweep_idle(&s, t0 + HEADER_TIMEOUT_SEC - 1);
    CHECK("UT-07 29초에는 그대로", conn_alive(&s, c));
    server_sweep_idle(&s, t0 + HEADER_TIMEOUT_SEC + 1);
    struct got g;
    recv_all(cfd, &g, 200);
    CHECK("UT-08 30초 넘으면 408", strncmp(g.buf, "HTTP/1.1 408 Request Timeout", 28) == 0);
    CHECK("UT-08 그리고 닫음", g.eof && s.nconn == 0);
    free(g.buf);
    close(cfd);

    /* UT-09: 아무것도 안 보낸 연결은 408이 아니라 유휴 15초로 조용히 닫힘 */
    c = connect_and_accept(&s, &cfd, 0);
    server_sweep_idle(&s, time(NULL) + HEADER_TIMEOUT_SEC + 1);
    recv_all(cfd, &g, 200);
    CHECK("UT-09 빈 연결은 응답 없이 닫힘", g.len == 0 && g.eof);
    free(g.buf);
    close(cfd);
    teardown(&s, lfd);
}

/* ---- 접근 로그 (S7-06) ---- */

static int g_logp[2] = {-1, -1};

static char *read_log(void)
{
    static char buf[16384];
    size_t len = 0;
    for (;;) {
        ssize_t n = read(g_logp[0], buf + len, sizeof(buf) - 1 - len);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    return buf;
}

static int count_lines(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += *s == '\n';
    return n;
}

/* "2026-11-26 00:20:01 " 모양인지 */
static int date_ok(const char *l)
{
    const char *f = "dddd-dd-dd dd:dd:dd ";
    for (int i = 0; f[i]; i++) {
        if (f[i] == 'd' ? (l[i] < '0' || l[i] > '9') : l[i] != f[i])
            return 0;
    }
    return 1;
}

static void ut10_to_ut14_log(void)
{
    if (pipe(g_logp) < 0) {
        CHECK("UT-10 준비", 0);
        return;
    }
    fcntl(g_logp[0], F_SETFL, O_NONBLOCK);
    access_log_set_fd(g_logp[1]);
    struct server s;
    int lfd;
    if (setup(&s, &lfd) < 0) {
        CHECK("UT-10 준비", 0);
        return;
    }
    int cfd;
    struct conn *c = connect_and_accept(&s, &cfd, 0);
    send_str(cfd, GET("/index.html"));
    readable(&s, c);
    struct got g;
    recv_all(cfd, &g, 100);
    char *log = read_log();
    char want[128];
    snprintf(want, sizeof(want), " 127.0.0.1 \"GET /index.html HTTP/1.1\" 200 %zu ", g.len);
    CHECK("UT-10 한 줄", count_lines(log) == 1);
    CHECK("UT-10 날짜 시각", date_ok(log));
    CHECK("UT-10 IP, 요청 줄, 상태, 보낸 바이트", strstr(log, want) != NULL);
    CHECK("UT-10 처리 시간 ms로 끝남", strstr(log, "ms\n") != NULL);
    free(g.buf);

    /* UT-11: keep-alive로 두 요청 → 두 줄, 404도 남김 */
    send_str(cfd, GET("/style.css") GET("/nope.html"));
    readable(&s, c);
    recv_all(cfd, &g, 100);
    log = read_log();
    CHECK("UT-11 요청 두 개 → 두 줄", count_lines(log) == 2);
    CHECK("UT-11 404도 남김", strstr(log, "\"GET /nope.html HTTP/1.1\" 404 ") != NULL);
    free(g.buf);

    /* UT-12: 408은 요청 줄 "-" */
    send_str(cfd, "GET /x HTTP/1.1\r\n");
    readable(&s, c);
    server_sweep_idle(&s, time(NULL) + HEADER_TIMEOUT_SEC + 1);
    recv_all(cfd, &g, 100);
    log = read_log();
    CHECK("UT-12 408은 요청 줄 \"-\"", strstr(log, " \"-\" 408 ") != NULL && count_lines(log) == 1);
    free(g.buf);
    close(cfd);

    /* UT-13: 응답 도중 끊겨도 한 줄 (보낸 만큼) */
    c = connect_and_accept(&s, &cfd, 4096);
    int small = 4096;
    setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    send_str(cfd, GET("/big.bin"));
    readable(&s, c);
    close(cfd);
    for (int i = 0; i < 50 && conn_alive(&s, c); i++) {
        sleep_ms(10);
        conn_on_writable(&s, c);
    }
    log = read_log();
    CHECK("UT-13 끊겨도 한 줄", count_lines(log) == 1 && strstr(log, "\"GET /big.bin HTTP/1.1\" 200 ") != NULL);
    long bytes = 0;
    const char *p = strstr(log, "\" 200 ");
    if (p != NULL)
        bytes = atol(p + 6);
    CHECK("UT-13 보낸 바이트는 1MB보다 적음", bytes > 0 && bytes < BIG_SIZE);

    /* UT-14: 끄면 아무것도 안 씀 */
    access_log_set_fd(-1);
    c = connect_and_accept(&s, &cfd, 0);
    send_str(cfd, GET("/index.html"));
    readable(&s, c);
    recv_all(cfd, &g, 100);
    CHECK("UT-14 fd -1이면 로그 없음", read_log()[0] == '\0');
    free(g.buf);
    close(cfd);
    teardown(&s, lfd);
    close(g_logp[0]);
    close(g_logp[1]);
}

/* ---- fd 바닥 (S7-05) ---- */

static void ut15_ut16_emfile(void)
{
    struct server s;
    int lfd;
    if (setup(&s, &lfd) < 0) {
        CHECK("UT-15 준비", 0);
        return;
    }
    struct rlimit old, low;
    getrlimit(RLIMIT_NOFILE, &old);
    low = old;
    low.rlim_cur = 256;                                  /* 잠깐 한도를 낮춰서 바닥을 흉내 냄 */
    if (old.rlim_cur > 256)
        setrlimit(RLIMIT_NOFILE, &low);
    int cfd = client_connect(0);                         /* 손님은 accept 큐에서 기다림 */
    int fill[4096], nfill = 0;
    for (;;) {                                           /* 남은 fd를 다 써 버림 */
        int d = dup(0);
        if (d < 0)
            break;
        if (nfill < 4096)
            fill[nfill++] = d;
        else {
            close(d);
            break;
        }
    }
    int got_emfile = errno == EMFILE;
    if (!got_emfile || nfill == 4096) {
        for (int i = 0; i < nfill; i++)
            close(fill[i]);
        setrlimit(RLIMIT_NOFILE, &old);
        printf("  skip  UT-15 fd 한도가 너무 커서 흉내 못 냄\n");
        close(cfd);
        teardown(&s, lfd);
        return;
    }
    server_poll_once(&s, 500);
    CHECK("UT-15 accept 실패 → 쉬기 시작", s.accept_paused == 1 && s.nconn == 0);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int n = server_poll_once(&s, 200);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK("UT-15 쉬는 동안은 깨지 않음 (헛돌지 않음)", n == 0 && ms >= 150);

    for (int i = 0; i < nfill; i++)
        close(fill[i]);
    setrlimit(RLIMIT_NOFILE, &old);
    server_sweep_idle(&s, time(NULL) + ACCEPT_PAUSE_SEC);
    CHECK("UT-16 1초 뒤 다시 감시", s.accept_paused == 0);
    server_poll_once(&s, 500);
    CHECK("UT-16 기다리던 손님을 받음", s.nconn == 1);
    close(cfd);
    teardown(&s, lfd);
}

/* ---- Nagle 끄기 ---- */

static void ut21_nodelay(void)
{
    struct server s;
    int lfd;
    if (setup(&s, &lfd) < 0) {
        CHECK("UT-21 준비", 0);
        return;
    }
    int cfd;
    struct conn *c = connect_and_accept(&s, &cfd, 0);
    int v = 0;
    socklen_t len = sizeof(v);
    CHECK("UT-21 받은 소켓은 TCP_NODELAY", c != NULL && getsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &v, &len) == 0 && v != 0);
    close(cfd);
    teardown(&s, lfd);
}

/* ---- 진짜 신호 (S7-01 ~ S7-03) ---- */

static int g_port2;

/* 자식에서 server_run(워커 2개). 첫 요청에 응답이 오면(= 신호를 막은 뒤) pid를 돌려줌 */
static pid_t start_server(void)
{
    int lfd = server_listen(0);
    if (lfd < 0)
        return -1;
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    getsockname(lfd, (struct sockaddr *)&a, &len);
    g_port2 = ntohs(a.sin_port);
    pid_t pid = fork();
    if (pid == 0) {
        server_run(lfd, g_root, NULL, 2);
        close(lfd);
        _exit(0);
    }
    close(lfd);
    int save = g_port;
    g_port = g_port2;
    int fd = client_connect(0);
    g_port = save;
    if (fd < 0)
        return pid;
    send_str(fd, "GET /index.html HTTP/1.1\r\nConnection: close\r\n\r\n");
    struct got g;
    recv_all(fd, &g, 2000);
    free(g.buf);
    close(fd);
    return pid;
}

static int connect2(int rcvbuf)
{
    int save = g_port;
    g_port = g_port2;
    int fd = client_connect(rcvbuf);
    g_port = save;
    return fd;
}

/* 자식이 끝날 때까지 최대 limit_ms 기다림. 걸린 ms, 안 끝나면 -1 (그때는 SIGKILL) */
static long wait_exit(pid_t pid, int limit_ms, int *status)
{
    for (int t = 0; t <= limit_ms; t += 20) {
        if (waitpid(pid, status, WNOHANG) == pid)
            return t;
        sleep_ms(20);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return -1;
}

static void ut17_to_ut20_signal(void)
{
    int st;
    /* UT-17: 쉬는 keep-alive 연결이 있어도 SIGTERM이면 바로(1초 안) 정상 종료 */
    pid_t pid = start_server();
    int fd = connect2(0);
    send_str(fd, GET("/index.html"));
    sleep_ms(100);
    kill(pid, SIGTERM);
    long ms = wait_exit(pid, 3000, &st);
    CHECK("UT-17 SIGTERM → 1초 안에 종료", ms >= 0 && ms < 1000);
    CHECK("UT-17 종료 코드 0 (신호로 죽지 않음)", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(fd);

    /* UT-18: 헤더를 다 안 보낸 클라이언트가 있으면 5초까지 기다렸다가 끝남 */
    pid = start_server();
    fd = connect2(0);
    send_str(fd, "GET / HTTP/1.1\r\n");
    sleep_ms(100);
    kill(pid, SIGINT);
    ms = wait_exit(pid, 8000, &st);
    CHECK("UT-18 SIGINT, 하던 요청이 안 끝나면 약 5초 뒤 종료", ms >= (SHUTDOWN_GRACE_SEC - 1) * 1000 && ms < (SHUTDOWN_GRACE_SEC + 2) * 1000);
    CHECK("UT-18 종료 코드 0", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(fd);

    /* UT-19: 두 번째 신호면 바로 끝남 */
    pid = start_server();
    fd = connect2(0);
    send_str(fd, "GET / HTTP/1.1\r\n");
    sleep_ms(100);
    kill(pid, SIGINT);
    sleep_ms(200);
    kill(pid, SIGINT);
    ms = wait_exit(pid, 3000, &st);
    CHECK("UT-19 두 번째 SIGINT → 1초 안에 종료", ms >= 0 && ms < 1000);
    CHECK("UT-19 종료 코드 130", WIFEXITED(st) && WEXITSTATUS(st) == 130);
    close(fd);

    /* UT-20: 보내던 1MB는 신호 뒤에도 끝까지 */
    pid = start_server();
    fd = connect2(4096);
    send_str(fd, GET("/big.bin"));
    sleep_ms(100);
    kill(pid, SIGTERM);
    struct got g;
    recv_all(fd, &g, 2000);
    CHECK("UT-20 신호 뒤에도 1MB를 끝까지 받음", body_is(&g, g_big, BIG_SIZE));
    CHECK("UT-20 그 뒤 닫음", g.eof);
    free(g.buf);
    close(fd);
    ms = wait_exit(pid, 3000, &st);
    CHECK("UT-20 그리고 종료", ms >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (!make_fixture()) {
        printf("테스트 루트를 못 만듦\n");
        return 1;
    }
    int fds0 = count_open_fds();
    ut01_to_ut04_stop();
    ut05_should_exit();
    ut06_stop_fd();
    ut07_to_ut09_header_timeout();
    ut10_to_ut14_log();
    ut15_ut16_emfile();
    ut21_nodelay();
    ut17_to_ut20_signal();
    CHECK("열린 fd 수가 처음과 같음", count_open_fds() == fds0);
    remove_fixture();
    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
