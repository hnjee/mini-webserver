/*
 * 이벤트 단위 테스트: docs/tests/step5.md 3장 UT-01 ~ UT-24
 * epoll_wait()은 부르지 않고, 알림이 왔다고 치고 server_on_accept(), conn_on_readable(),
 * conn_on_writable(), server_sweep_idle()을 직접 불러 상태를 본다.
 * 한 프로세스 안에서 포트 0으로 리슨 소켓을 열고, 같은 프로세스에서 클라이언트 소켓으로 접속한다.
 * 클라이언트 소켓은 논블로킹이라, 응답이 없으면 read()가 EAGAIN으로 바로 돌아온다.
 */
/* realpath()는 POSIX의 XSI 부분이라 _XOPEN_SOURCE를 켜야 선언된다 */
#define _XOPEN_SOURCE 700

#include "server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

static char g_base[] = "/tmp/ws_event_XXXXXX";
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

static int count_resp(const struct got *g)
{
    int n = 0;
    for (const char *p = g->buf; (p = strstr(p, "HTTP/1.1 ")) != NULL; p += 9)
        n++;
    return n;
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

/* ---- 연결 받기 ---- */

static void ut01_to_ut03_accept(void)
{
    struct server s;
    int lfd;
    if (setup(&s, &lfd) < 0) {
        CHECK("UT-01 server_init", 0);
        return;
    }
    CHECK("UT-01 접속 없음 → 0", server_on_accept(&s) == 0);
    CHECK("UT-01 nconn == 0", s.nconn == 0);

    int c[100];
    for (int i = 0; i < 3; i++)
        c[i] = client_connect(0);
    struct pollfd p = {lfd, POLLIN, 0};
    poll(&p, 1, 1000);
    CHECK("UT-02 3개 접속 → 3", server_on_accept(&s) == 3);
    CHECK("UT-02 nconn == 3", s.nconn == 3);
    int nb = 1, st = 1;
    for (struct conn *x = s.conns; x != NULL; x = x->next) {
        nb &= (fcntl(x->fd, F_GETFL) & O_NONBLOCK) != 0;
        st &= x->state == ST_HEADER;
    }
    CHECK("UT-02 모든 연결 fd가 O_NONBLOCK", nb);
    CHECK("UT-02 모든 연결 state == ST_HEADER", st);
    for (int i = 0; i < 3; i++)
        close(c[i]);
    teardown(&s, lfd);

    if (setup(&s, &lfd) < 0)
        return;
    for (int i = 0; i < 100; i++)
        c[i] = client_connect(0);
    int a = server_on_accept(&s), b = server_on_accept(&s), z = server_on_accept(&s);
    CHECK("UT-03 100개 접속 → 64, 36, 0", a == 64 && b == 36 && z == 0);
    for (int i = 0; i < 100; i++)
        close(c[i]);
    teardown(&s, lfd);
}

/* ---- 읽기 ---- */

static void ut04_to_ut05_split_header(void)
{
    struct server s;
    int lfd, cfd;
    if (setup(&s, &lfd) < 0)
        return;
    struct conn *c = connect_and_accept(&s, &cfd, 0);
    CHECK("UT-04 연결 받음", c != NULL);
    if (c == NULL) {
        teardown(&s, lfd);
        return;
    }
    const char *part1 = "GET /index.html HTTP/1.1\r\nHo";
    send_str(cfd, part1);
    readable(&s, c);
    struct got g;
    recv_all(cfd, &g, 100);
    CHECK("UT-04 state == ST_HEADER", c->state == ST_HEADER);
    CHECK("UT-04 len == 보낸 바이트 수", c->len == strlen(part1));
    CHECK("UT-04 응답 없음", g.len == 0 && !g.eof);
    free(g.buf);

    send_str(cfd, "st: x\r\n\r\n");
    readable(&s, c);
    recv_all(cfd, &g, 200);
    CHECK("UT-05 200 OK", strncmp(g.buf, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK("UT-05 본문이 index.html", body_is(&g, INDEX_HTML, sizeof(INDEX_HTML) - 1));
    CHECK("UT-05 Connection: keep-alive", has(&g, "\r\nConnection: keep-alive\r\n"));
    CHECK("UT-05 state == ST_HEADER, len == 0", conn_alive(&s, c) && c->state == ST_HEADER && c->len == 0);
    free(g.buf);
    close(cfd);
    teardown(&s, lfd);
}

static void ut06_to_ut07_interleave(void)
{
    struct server s;
    int lfd, fa, fb;
    if (setup(&s, &lfd) < 0)
        return;
    struct conn *a = connect_and_accept(&s, &fa, 0);
    struct conn *b = connect_and_accept(&s, &fb, 0);
    if (a == NULL || b == NULL) {
        CHECK("UT-06 연결 두 개 받음", 0);
        teardown(&s, lfd);
        return;
    }
    send_str(fa, "GET /index.html HTTP/1.1\r\nHo");
    send_str(fb, GET("/style.css"));
    readable(&s, a);
    readable(&s, b);
    struct got ga, gb;
    recv_all(fa, &ga, 100);
    recv_all(fb, &gb, 200);
    CHECK("UT-06 B만 200", strncmp(gb.buf, "HTTP/1.1 200 OK\r\n", 17) == 0 && body_is(&gb, STYLE_CSS, sizeof(STYLE_CSS) - 1));
    CHECK("UT-06 A는 응답 없음", ga.len == 0 && !ga.eof);
    free(ga.buf);
    free(gb.buf);

    send_str(fa, "st: x\r\n\r\n");
    readable(&s, a);
    recv_all(fa, &ga, 200);
    CHECK("UT-07 A가 200, 본문은 index.html", strncmp(ga.buf, "HTTP/1.1 200 OK\r\n", 17) == 0 &&
                                                body_is(&ga, INDEX_HTML, sizeof(INDEX_HTML) - 1));
    free(ga.buf);
    close(fa);
    close(fb);
    teardown(&s, lfd);
}

static void ut08_to_ut09_split_body(void)
{
    struct server s;
    int lfd, cfd;
    if (setup(&s, &lfd) < 0)
        return;
    struct conn *c = connect_and_accept(&s, &cfd, 0);
    if (c == NULL) {
        CHECK("UT-08 연결 받음", 0);
        teardown(&s, lfd);
        return;
    }
    send_str(cfd, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\nname=");
    readable(&s, c);
    struct got g;
    recv_all(cfd, &g, 100);
    CHECK("UT-08 state == ST_BODY", c->state == ST_BODY);
    CHECK("UT-08 응답 없음", g.len == 0);
    free(g.buf);

    send_str(cfd, "hyunji");
    readable(&s, c);
    recv_all(cfd, &g, 200);
    CHECK("UT-09 본문 name=hyunji", body_is(&g, "name=hyunji", 11));
    CHECK("UT-09 state == ST_HEADER", conn_alive(&s, c) && c->state == ST_HEADER);
    free(g.buf);
    close(cfd);
    teardown(&s, lfd);
}

static void ut10_to_ut14_close(void)
{
    struct server s;
    int lfd, cfd;
    struct got g;
    if (setup(&s, &lfd) < 0)
        return;

    /* UT-10 파이프라이닝 */
    struct conn *c = connect_and_accept(&s, &cfd, 0);
    if (c != NULL) {
        send_str(cfd, GET("/style.css") GET("/index.html"));
        readable(&s, c);
        recv_all(cfd, &g, 200);
        const char *second = strstr(g.buf + 1, "HTTP/1.1 ");
        CHECK("UT-10 응답 2개", count_resp(&g) == 2);
        CHECK("UT-10 순서: style.css 다음 index.html",
              second != NULL && strstr(g.buf, STYLE_CSS) < second && strstr(second, INDEX_HTML) != NULL);
        free(g.buf);
        close(cfd);
    }

    /* UT-11 Connection: close */
    c = connect_and_accept(&s, &cfd, 0);
    size_t before = s.nconn;
    if (c != NULL) {
        send_str(cfd, "GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        readable(&s, c);
        recv_all(cfd, &g, 200);
        CHECK("UT-11 응답 뒤 연결이 닫힘 (read == 0)", count_resp(&g) == 1 && g.eof);
        CHECK("UT-11 nconn이 하나 줄어듦", s.nconn == before - 1);
        free(g.buf);
        close(cfd);
    }

    /* UT-12 501은 닫음 */
    c = connect_and_accept(&s, &cfd, 0);
    if (c != NULL) {
        send_str(cfd, "BREW / HTTP/1.1\r\nHost: x\r\n\r\n");
        readable(&s, c);
        recv_all(cfd, &g, 200);
        CHECK("UT-12 501, Connection: close, 닫힘",
              strncmp(g.buf, "HTTP/1.1 501 ", 13) == 0 && has(&g, "\r\nConnection: close\r\n") && g.eof);
        free(g.buf);
        close(cfd);
    }
    teardown(&s, lfd);

    /* UT-13 아무것도 안 보내고 끊음 */
    if (setup(&s, &lfd) < 0)
        return;
    int base = count_open_fds();
    c = connect_and_accept(&s, &cfd, 0);
    if (c != NULL) {
        close(cfd);
        readable(&s, c);
        CHECK("UT-13 nconn == 0", s.nconn == 0);
        CHECK("UT-13 열린 fd 수가 접속 전과 같음", count_open_fds() == base);
    }

    /* UT-14 요청 앞부분만 보내고 끊음 */
    c = connect_and_accept(&s, &cfd, 0);
    if (c != NULL) {
        send_str(cfd, "GET /index.html HTTP/1.1\r\n");
        readable(&s, c);
        close(cfd);
        if (conn_alive(&s, c))
            readable(&s, c);
        CHECK("UT-14 nconn == 0", s.nconn == 0);
    }
    teardown(&s, lfd);
}

/* ---- 보내기 ---- */

/* 응답 전체(헤더 + 본문)에서 본문만 떼어 big.bin과 같은지 */
static int big_matches(const char *buf, size_t len)
{
    const char *h = strstr(buf, "\r\n\r\n");
    if (h == NULL)
        return 0;
    size_t off = (size_t)(h + 4 - buf);
    return len - off == BIG_SIZE && memcmp(buf + off, g_big, BIG_SIZE) == 0;
}

static void ut15_to_ut18_write(void)
{
    struct server s;
    int lfd, cfd, fb;
    if (setup(&s, &lfd) < 0)
        return;
    struct conn *c = connect_and_accept(&s, &cfd, 4096);
    if (c == NULL) {
        CHECK("UT-15 연결 받음", 0);
        teardown(&s, lfd);
        return;
    }
    int small = 4096;
    setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));

    send_str(cfd, GET("/big.bin"));
    readable(&s, c);
    CHECK("UT-15 멈추지 않고 돌아옴, state == ST_WRITE", c->state == ST_WRITE);
    CHECK("UT-15 want_out == 1", c->want_out == 1);
    CHECK("UT-15 0 < file_off < file_size", c->file_off > 0 && c->file_off < c->file_size);

    struct conn *b = connect_and_accept(&s, &fb, 0);
    if (b != NULL) {
        send_str(fb, GET("/index.html"));
        readable(&s, b);
        struct got g;
        recv_all(fb, &g, 200);
        CHECK("UT-16 A가 보내는 중이어도 B는 바로 200", strncmp(g.buf, "HTTP/1.1 200 OK\r\n", 17) == 0);
        free(g.buf);
        close(fb);
    }

    /* UT-17 클라이언트가 읽는 대로 conn_on_writable()을 반복 */
    size_t cap = BIG_SIZE + 65536, got = 0;
    char *all = malloc(cap);
    for (int i = 0; all != NULL && i < 100000 && conn_alive(&s, c) && c->state == ST_WRITE; i++) {
        ssize_t n = read(cfd, all + got, cap - got);
        if (n > 0)
            got += (size_t)n;
        struct pollfd p = {c->fd, POLLOUT, 0};
        if (poll(&p, 1, 100) > 0)
            conn_on_writable(&s, c);
    }
    for (;;) {
        struct pollfd p = {cfd, POLLIN, 0};
        if (all == NULL || poll(&p, 1, 200) <= 0)
            break;
        ssize_t n = read(cfd, all + got, cap - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    CHECK("UT-17 받은 본문이 big.bin과 같음", all != NULL && big_matches(all, got));
    CHECK("UT-17 끝나면 want_out == 0, state == ST_HEADER, file_fd == -1",
          conn_alive(&s, c) && c->want_out == 0 && c->state == ST_HEADER && c->file_fd == -1);
    free(all);

    /* UT-18 작은 응답은 한 번에 */
    send_str(cfd, GET("/nope.html"));
    readable(&s, c);
    CHECK("UT-18 404를 한 번에 보냄, want_out == 0", conn_alive(&s, c) && c->want_out == 0 && c->state == ST_HEADER);
    close(cfd);
    teardown(&s, lfd);
}

/* ---- 유휴 연결 ---- */

static void ut19_to_ut22_idle(void)
{
    struct server s;
    int lfd, fa, fb;
    if (setup(&s, &lfd) < 0)
        return;
    struct conn *a = connect_and_accept(&s, &fa, 0);
    if (a == NULL) {
        CHECK("UT-19 연결 받음", 0);
        teardown(&s, lfd);
        return;
    }
    a->last_active = time(NULL) - 100;
    send_str(fa, "G");
    readable(&s, a);
    CHECK("UT-19 바이트가 오면 last_active가 지금으로", a->last_active >= time(NULL) - 1);

    time_t t = a->last_active;
    CHECK("UT-20 14초 뒤 → 0, 그대로", server_sweep_idle(&s, t + 14) == 0 && conn_alive(&s, a));
    CHECK("UT-21 15초 뒤 → 1", server_sweep_idle(&s, t + 15) == 1);
    struct got g;
    recv_all(fa, &g, 200);
    CHECK("UT-21 클라이언트 read == 0", g.eof);
    free(g.buf);
    close(fa);

    a = connect_and_accept(&s, &fa, 0);
    struct conn *b = connect_and_accept(&s, &fb, 0);
    if (a != NULL && b != NULL) {
        a->last_active = time(NULL) - 20;
        CHECK("UT-22 A만 닫힘", server_sweep_idle(&s, time(NULL)) == 1 && !conn_alive(&s, a) && conn_alive(&s, b));
    }
    close(fa);
    close(fb);
    teardown(&s, lfd);
}

/* ---- 정리 ---- */

static void ut23_free(void)
{
    struct server s;
    int lfd;
    int before = count_open_fds();   /* 리슨 소켓 열기 전 */
    if (setup(&s, &lfd) < 0)
        return;
    int cf[5];
    struct conn *c[5];
    for (int i = 0; i < 5; i++)
        c[i] = connect_and_accept(&s, &cf[i], i == 0 ? 4096 : 0);
    if (c[0] != NULL) {
        int small = 4096;
        setsockopt(c[0]->fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
        send_str(cf[0], GET("/big.bin"));
        readable(&s, c[0]);
    }
    server_free(&s);
    for (int i = 0; i < 5; i++)
        close(cf[i]);
    close(lfd);
    CHECK("UT-23 server_free 뒤 열린 fd 수가 처음과 같음", count_open_fds() == before);
}

/* ---- 동시에 보낸 본문 (S5-07) ---- */

static void ut24_bodies(void)
{
    struct server s;
    int lfd, fa, fb;
    if (setup(&s, &lfd) < 0)
        return;
    struct conn *a = connect_and_accept(&s, &fa, 0);
    struct conn *b = connect_and_accept(&s, &fb, 0);
    if (a == NULL || b == NULL) {
        CHECK("UT-24 연결 두 개 받음", 0);
        teardown(&s, lfd);
        return;
    }
    send_str(fa, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 8\r\n\r\nAAAA");
    send_str(fb, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 8\r\n\r\nBBBB");
    readable(&s, a);
    readable(&s, b);
    send_str(fb, "BBBB");
    readable(&s, b);
    send_str(fa, "AAAA");
    readable(&s, a);
    struct got ga, gb;
    recv_all(fa, &ga, 200);
    recv_all(fb, &gb, 200);
    CHECK("UT-24 A는 AAAAAAAA만", body_is(&ga, "AAAAAAAA", 8));
    CHECK("UT-24 B는 BBBBBBBB만", body_is(&gb, "BBBBBBBB", 8));
    free(ga.buf);
    free(gb.buf);
    close(fa);
    close(fb);
    teardown(&s, lfd);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    alarm(60);   /* 이벤트 함수가 멈추면 테스트를 끝냄 */

    printf("test_event\n");
    if (!make_fixture()) {
        printf("  FAIL  테스트 루트를 만들지 못함 (%s)\n", g_base);
        return 1;
    }
    ut01_to_ut03_accept();
    ut04_to_ut05_split_header();
    ut06_to_ut07_interleave();
    ut08_to_ut09_split_body();
    ut10_to_ut14_close();
    ut15_to_ut18_write();
    ut19_to_ut22_idle();
    ut23_free();
    ut24_bodies();
    remove_fixture();

    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
