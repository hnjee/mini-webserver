/*
 * 프록시 단위 테스트: docs/tests/step6.md 4장 (S6-02 ~ S6-10)
 * 한 프로세스 안에서 서버(struct server), 클라이언트, 가짜 백엔드를 모두 만든다.
 * - 서버: server_init() 뒤 s.routes에 라우팅 표를 넣고, server_poll_once()로 알림을 처리한다.
 * - 가짜 백엔드: 테스트가 연 리슨 소켓. accept(), 요청 읽기, 응답 쓰기, 닫기를 테스트가 직접 한다.
 * - 시간 제한은 기다리지 않고 server_sweep_idle(s, 지금 + N)으로 시각을 건너뛰어 확인한다.
 */
#define _XOPEN_SOURCE 700

#include "route.h"
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

/* ---- 테스트 루트와 라우팅 표 ---- */

static char g_base[] = "/tmp/ws_proxy_XXXXXX";
static char g_root[PATH_MAX];
static const char INDEX_HTML[] = "<h1>hi</h1>\n";
static struct routes g_routes;

static int make_fixture(void)
{
    char www[PATH_MAX], p[PATH_MAX + 16];
    if (mkdtemp(g_base) == NULL)
        return 0;
    snprintf(www, sizeof(www), "%s/www", g_base);
    if (mkdir(www, 0755) < 0)
        return 0;
    snprintf(p, sizeof(p), "%s/index.html", www);
    FILE *f = fopen(p, "w");
    if (f == NULL)
        return 0;
    fputs(INDEX_HTML, f);
    fclose(f);
    return realpath(www, g_root) != NULL;
}

static void remove_fixture(void)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0)
        printf("  (임시 폴더 %s를 지우지 못함)\n", g_base);
}

/* "/api/ 127.0.0.1:p1 127.0.0.1:p2"로 라우팅 표를 만든다 (p2가 0이면 백엔드 하나) */
static int set_routes(int p1, int p2)
{
    char conf[PATH_MAX + 32], line[128];
    snprintf(conf, sizeof(conf), "%s/routes.conf", g_base);
    if (p2 > 0)
        snprintf(line, sizeof(line), "/api/ 127.0.0.1:%d 127.0.0.1:%d\n", p1, p2);
    else
        snprintf(line, sizeof(line), "/api/ 127.0.0.1:%d\n", p1);
    FILE *f = fopen(conf, "w");
    if (f == NULL)
        return -1;
    fputs(line, f);
    fclose(f);
    memset(&g_routes, 0, sizeof(g_routes));
    return routes_load(conf, &g_routes);
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

static int port_of_fd(int fd)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &len);
    return ntohs(a.sin_port);
}

static int g_port;

static int setup(struct server *s, int *lfd)
{
    *lfd = server_listen(0);
    if (*lfd < 0)
        return -1;
    g_port = port_of_fd(*lfd);
    if (server_init(s, *lfd, g_root) < 0)
        return -1;
    s->routes = &g_routes;
    return 0;
}

static void teardown(struct server *s, int lfd)
{
    server_free(s);
    close(lfd);
}

/* 가짜 백엔드의 리슨 소켓 (블로킹). 포트는 *port에 */
static int backend_listen(int *port)
{
    int fd = server_listen(0);
    if (fd >= 0)
        *port = port_of_fd(fd);
    return fd;
}

/* 비어 있는 포트 하나 (열었다 바로 닫음) */
static int closed_port(void)
{
    int port = 0;
    int fd = backend_listen(&port);
    if (fd >= 0)
        close(fd);
    return port;
}

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

static void send_str(int fd, const char *str, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, str + sent, len - sent);
        if (n > 0)
            sent += (size_t)n;
        else if (n < 0 && errno != EAGAIN && errno != EINTR)
            return;
        else {
            struct pollfd p = {fd, POLLOUT, 0};
            poll(&p, 1, 100);
        }
    }
}
#define SEND(fd, lit) send_str((fd), (lit), strlen(lit))

/* 서버 알림을 times번 처리한다 (한 번에 최대 20ms 기다림) */
static void pump(struct server *s, int times)
{
    for (int i = 0; i < times; i++)
        server_poll_once(s, 20);
}

/* 가짜 백엔드: 손님이 올 때까지 서버를 돌리며 기다렸다 accept. 실패하면 -1 */
static int backend_accept(struct server *s, int blfd)
{
    for (int i = 0; i < 100; i++) {
        struct pollfd p = {blfd, POLLIN, 0};
        if (poll(&p, 1, 0) > 0)
            return accept(blfd, NULL, NULL);
        server_poll_once(s, 20);
    }
    return -1;
}

/* 가짜 백엔드: 요청을 헤더의 끝 + Content-Length까지 읽는다. 서버를 돌리며 기다린다 */
static size_t backend_read_request(struct server *s, int bfd, char *buf, size_t cap)
{
    size_t got = 0;
    fcntl(bfd, F_SETFL, fcntl(bfd, F_GETFL) | O_NONBLOCK);
    for (int i = 0; i < 200 && got < cap - 1; i++) {
        ssize_t n = read(bfd, buf + got, cap - 1 - got);
        if (n > 0) {
            got += (size_t)n;
            buf[got] = '\0';
            char *end = strstr(buf, "\r\n\r\n");
            if (end != NULL) {
                char *cl = strstr(buf, "Content-Length: ");
                size_t want = (size_t)(end + 4 - buf) + (cl != NULL && cl < end ? strtoul(cl + 16, NULL, 10) : 0);
                if (got >= want)
                    break;
            }
            continue;
        }
        if (n == 0)
            break;
        server_poll_once(s, 20);
    }
    buf[got] = '\0';
    return got;
}

/* 클라이언트가 받은 것 */
struct got {
    char *buf;
    size_t len;
    int eof;
};

#define BIG (1024 * 1024)

/* 서버를 돌리면서 클라이언트가 받을 수 있는 만큼 받는다. 끊기거나 rounds번 동안 아무것도 안 오면 멈춘다 */
static void client_recv(struct server *s, int cfd, struct got *g, int rounds)
{
    size_t cap = BIG + 65536;
    g->buf = malloc(cap + 1);
    g->len = 0;
    g->eof = 0;
    int quiet = 0;
    while (g->buf != NULL && quiet < rounds) {
        server_poll_once(s, 20);
        ssize_t n = read(cfd, g->buf + g->len, cap - g->len);
        if (n > 0) {
            g->len += (size_t)n;
            quiet = 0;
            continue;
        }
        if (n == 0) {
            g->eof = 1;
            break;
        }
        if (errno == ECONNRESET) {
            g->eof = 1;
            break;
        }
        quiet++;
    }
    if (g->buf != NULL)
        g->buf[g->len] = '\0';
}

static int starts(const struct got *g, const char *p)
{
    return g->buf != NULL && strncmp(g->buf, p, strlen(p)) == 0;
}

/* ---- 테스트 ---- */

static const char RESP_8001[] = "HTTP/1.0 200 OK\r\nContent-Length: 4\r\n\r\n8001";

/* UT-14 ~ UT-16: 요청 넘기기, 응답 그대로, 클라이언트 닫기 */
static void ut14_to_ut16_forward(void)
{
    struct server s;
    int lfd, bport, cfd;
    int blfd = backend_listen(&bport);
    if (blfd < 0 || set_routes(bport, 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-14 준비", 0);
        return;
    }
    cfd = client_connect(0);
    SEND(cfd, "GET /api/x?y=1 HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nX-Test: 1\r\n\r\n");
    int bfd = backend_accept(&s, blfd);
    char req[4096] = "";
    if (bfd >= 0)
        backend_read_request(&s, bfd, req, sizeof(req));
    CHECK("UT-14 백엔드가 접속을 받음", bfd >= 0);
    CHECK("UT-14 요청 줄 그대로 (쿼리 포함)", strncmp(req, "GET /api/x?y=1 HTTP/1.1\r\n", 25) == 0);
    CHECK("UT-14 Host 그대로", strstr(req, "\r\nHost: x\r\n") != NULL);
    CHECK("UT-14 다른 헤더 그대로", strstr(req, "\r\nX-Test: 1\r\n") != NULL);
    CHECK("UT-14 Connection: close로 바뀜", strstr(req, "\r\nConnection: close\r\n") != NULL && strstr(req, "keep-alive") == NULL);

    if (bfd >= 0) {
        SEND(bfd, RESP_8001);
        close(bfd);
    }
    struct got g;
    client_recv(&s, cfd, &g, 20);
    CHECK("UT-15 백엔드 응답이 바이트 그대로", g.len == strlen(RESP_8001) && memcmp(g.buf, RESP_8001, g.len) == 0);
    CHECK("UT-16 응답 뒤 클라이언트 연결이 닫힘", g.eof);
    CHECK("UT-16 nconn == 0", s.nconn == 0);
    free(g.buf);
    close(cfd);
    teardown(&s, lfd);
    close(blfd);
}

/* UT-17: POST 본문 넘기기 */
static void ut17_post_body(void)
{
    struct server s;
    int lfd, bport;
    int blfd = backend_listen(&bport);
    if (blfd < 0 || set_routes(bport, 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-17 준비", 0);
        return;
    }
    int cfd = client_connect(0);
    SEND(cfd, "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc");
    int bfd = backend_accept(&s, blfd);
    char req[4096] = "";
    if (bfd >= 0)
        backend_read_request(&s, bfd, req, sizeof(req));
    char *body = strstr(req, "\r\n\r\n");
    CHECK("UT-17 POST 요청 줄", strncmp(req, "POST /api/echo HTTP/1.1\r\n", 25) == 0);
    CHECK("UT-17 Content-Length: 3과 본문 abc", strstr(req, "\r\nContent-Length: 3\r\n") != NULL && body != NULL && strcmp(body + 4, "abc") == 0);
    if (bfd >= 0)
        close(bfd);
    close(cfd);
    teardown(&s, lfd);
    close(blfd);
}

/* UT-18: 라운드 로빈 (백엔드 둘, 요청 넷) */
static void ut18_round_robin(void)
{
    struct server s;
    int lfd, pa, pb;
    int la = backend_listen(&pa), lb = backend_listen(&pb);
    if (la < 0 || lb < 0 || set_routes(pa, pb) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-18 준비", 0);
        return;
    }
    char order[5] = "";
    for (int i = 0; i < 4; i++) {
        int cfd = client_connect(0);
        SEND(cfd, "GET /api/whoami.txt HTTP/1.1\r\nHost: x\r\n\r\n");
        int which = -1, bfd = -1;
        for (int k = 0; k < 100 && which < 0; k++) {
            server_poll_once(&s, 20);
            struct pollfd p[2] = {{la, POLLIN, 0}, {lb, POLLIN, 0}};
            if (poll(p, 2, 0) > 0)
                which = (p[0].revents & POLLIN) ? 0 : 1;
        }
        if (which >= 0) {
            bfd = accept(which == 0 ? la : lb, NULL, NULL);
            char req[2048];
            backend_read_request(&s, bfd, req, sizeof(req));
            SEND(bfd, which == 0 ? "HTTP/1.0 200 OK\r\n\r\nA" : "HTTP/1.0 200 OK\r\n\r\nB");
            close(bfd);
        }
        order[i] = which == 0 ? 'A' : which == 1 ? 'B' : '?';
        struct got g;
        client_recv(&s, cfd, &g, 10);
        free(g.buf);
        close(cfd);
    }
    CHECK("UT-18 A, B, A, B 순서", strcmp(order, "ABAB") == 0);
    teardown(&s, lfd);
    close(la);
    close(lb);
}

/* UT-19: 라우팅 표에 없는 경로는 정적 파일 그대로 (S6-09) */
static void ut19_static(void)
{
    struct server s;
    int lfd, bport;
    int blfd = backend_listen(&bport);
    if (blfd < 0 || set_routes(bport, 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-19 준비", 0);
        return;
    }
    int cfd = client_connect(0);
    SEND(cfd, "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n");
    struct got g;
    client_recv(&s, cfd, &g, 10);
    CHECK("UT-19 200, keep-alive, 연결 유지", starts(&g, "HTTP/1.1 200 OK\r\n") &&
                                              strstr(g.buf, "\r\nConnection: keep-alive\r\n") != NULL && !g.eof);
    struct pollfd p = {blfd, POLLIN, 0};
    CHECK("UT-19 백엔드로 안 감", poll(&p, 1, 0) == 0);
    free(g.buf);
    close(cfd);
    teardown(&s, lfd);
    close(blfd);
}

/* UT-20, UT-21: 큰 응답을 64KB 버퍼로 중계, 느린 클라이언트 (S6-04) */
static void ut20_to_ut21_large(void)
{
    struct server s;
    int lfd, bport;
    int blfd = backend_listen(&bport);
    if (blfd < 0 || set_routes(bport, 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-20 준비", 0);
        return;
    }
    int cfd = client_connect(4096);
    SEND(cfd, "GET /api/large.bin HTTP/1.1\r\nHost: x\r\n\r\n");
    int bfd = backend_accept(&s, blfd);
    char req[2048];
    if (bfd >= 0)
        backend_read_request(&s, bfd, req, sizeof(req));

    char *data = malloc(BIG);
    for (size_t i = 0; i < BIG; i++)
        data[i] = (char)((i * 31 + 7) % 251);
    const char *head = "HTTP/1.0 200 OK\r\nContent-Length: 1048576\r\n\r\n";
    size_t hl = strlen(head);

    /* 백엔드는 논블로킹으로 쓸 수 있는 만큼 쓰고, 서버와 클라이언트를 번갈아 돌린다 */
    fcntl(bfd, F_SETFL, fcntl(bfd, F_GETFL) | O_NONBLOCK);
    size_t bsent = 0, total = hl + BIG, cap = total + 1024, got = 0;
    char *all = malloc(cap);
    int max_relay_ok = 1, eof = 0;
    for (int i = 0; i < 200000 && !eof; i++) {
        if (bfd >= 0 && bsent < total) {
            const char *p = bsent < hl ? head + bsent : data + (bsent - hl);
            size_t n = bsent < hl ? hl - bsent : total - bsent;
            ssize_t w = write(bfd, p, n);
            if (w > 0)
                bsent += (size_t)w;
            if (bsent == total) {
                close(bfd);
                bfd = -1;
            }
        }
        server_poll_once(&s, 1);
        if (s.conns != NULL && s.conns->relay != NULL && s.conns->wpos > PROXY_RELAY_SIZE)
            max_relay_ok = 0;
        ssize_t r = read(cfd, all + got, cap - got);
        if (r > 0)
            got += (size_t)r;
        else if (r == 0)
            eof = 1;
    }
    const char *body = got > hl ? all + hl : NULL;
    CHECK("UT-20 1MB를 끝까지 바이트 그대로 받음", got == total && body != NULL && memcmp(body, data, BIG) == 0);
    CHECK("UT-21 중계 버퍼는 64KB를 넘지 않음", max_relay_ok);
    CHECK("UT-21 다 받은 뒤 닫힘, nconn == 0", eof && s.nconn == 0);
    free(all);
    free(data);
    if (bfd >= 0)
        close(bfd);
    close(cfd);
    teardown(&s, lfd);
    close(blfd);
}

/* UT-22: 연결 거절 → 502 (S6-06) */
static void ut22_refused(void)
{
    struct server s;
    int lfd;
    if (set_routes(closed_port(), 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-22 준비", 0);
        return;
    }
    int cfd = client_connect(0);
    SEND(cfd, "GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n");
    struct got g;
    client_recv(&s, cfd, &g, 20);
    CHECK("UT-22 502, Connection: close, 닫힘", starts(&g, "HTTP/1.1 502 Bad Gateway\r\n") &&
                                                 strstr(g.buf, "\r\nConnection: close\r\n") != NULL && g.eof);
    free(g.buf);
    close(cfd);
    teardown(&s, lfd);
}

/* UT-23: 연결이 3초 안에 안 맺어짐 → 504 (S6-07). accept 큐가 가득 찬 백엔드로 흉내 낸다 */
static void ut23_connect_timeout(void)
{
    struct server s;
    int lfd;
    int blfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(blfd, (struct sockaddr *)&a, sizeof(a));
    listen(blfd, 0);
    int bport = port_of_fd(blfd);
    a.sin_port = htons((unsigned short)bport);
    int fill[4];
    for (int i = 0; i < 4; i++) {        /* 줄을 채운다 (accept하지 않음) */
        fill[i] = socket(AF_INET, SOCK_STREAM, 0);
        fcntl(fill[i], F_SETFL, O_NONBLOCK);
        connect(fill[i], (struct sockaddr *)&a, sizeof(a));
    }
    struct timespec ts = {0, 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
    if (set_routes(bport, 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-23 준비", 0);
        return;
    }
    int cfd = client_connect(0);
    SEND(cfd, "GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&s, 10);
    if (s.conns == NULL || s.conns->state != ST_PROXY_CONNECT) {
        printf("  skip  UT-23 (백엔드 연결이 기다리지 않고 끝남: 이 커널에서는 흉내 낼 수 없음, FT-07로 확인)\n");
    } else {
        server_sweep_idle(&s, time(NULL) + s.connect_timeout + 1);
        struct got g;
        client_recv(&s, cfd, &g, 10);
        CHECK("UT-23 연결 시간 제한 → 504, 닫힘", starts(&g, "HTTP/1.1 504 ") && g.eof);
        free(g.buf);
    }
    close(cfd);
    teardown(&s, lfd);
    for (int i = 0; i < 4; i++)
        close(fill[i]);
    close(blfd);
}

/* UT-24 ~ UT-25: 응답 시간 제한 (S6-07) */
static void ut24_to_ut25_response_timeout(void)
{
    struct server s;
    int lfd, bport;
    int blfd = backend_listen(&bport);
    if (blfd < 0 || set_routes(bport, 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-24 준비", 0);
        return;
    }
    /* UT-24: 아무것도 안 보냄 */
    int cfd = client_connect(0);
    SEND(cfd, "GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n");
    int bfd = backend_accept(&s, blfd);
    char req[2048];
    if (bfd >= 0)
        backend_read_request(&s, bfd, req, sizeof(req));
    pump(&s, 3);
    CHECK("UT-24 8초 뒤에는 그대로", server_sweep_idle(&s, time(NULL) + s.response_timeout - 2) == 0 && s.nconn == 1);
    server_sweep_idle(&s, time(NULL) + s.response_timeout + 1);
    struct got g;
    client_recv(&s, cfd, &g, 10);
    CHECK("UT-24 11초 뒤 504, 닫힘", starts(&g, "HTTP/1.1 504 Gateway Timeout\r\n") && g.eof);
    char t;
    CHECK("UT-24 백엔드 연결도 닫힘", bfd >= 0 && read(bfd, &t, 1) == 0);
    free(g.buf);
    close(cfd);
    if (bfd >= 0)
        close(bfd);

    /* UT-25: 일부 보내고 멈춤 → 504 없이 닫기 */
    cfd = client_connect(0);
    SEND(cfd, "GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n");
    bfd = backend_accept(&s, blfd);
    if (bfd >= 0) {
        backend_read_request(&s, bfd, req, sizeof(req));
        SEND(bfd, "HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\nabc");
    }
    client_recv(&s, cfd, &g, 5);
    server_sweep_idle(&s, time(NULL) + s.response_timeout + 1);
    struct got g2;
    client_recv(&s, cfd, &g2, 10);
    CHECK("UT-25 받은 데까지 넘긴 뒤 504 없이 닫힘", starts(&g, "HTTP/1.0 200 OK") && strstr(g2.buf, "504") == NULL && (g.eof || g2.eof));
    free(g.buf);
    free(g2.buf);
    close(cfd);
    if (bfd >= 0)
        close(bfd);
    teardown(&s, lfd);
    close(blfd);
}

/* UT-26: 백엔드가 한 바이트도 없이 닫음 → 502 (S6-10) */
static void ut26_empty(void)
{
    struct server s;
    int lfd, bport;
    int blfd = backend_listen(&bport);
    if (blfd < 0 || set_routes(bport, 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-26 준비", 0);
        return;
    }
    int cfd = client_connect(0);
    SEND(cfd, "GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n");
    int bfd = backend_accept(&s, blfd);
    char req[2048];
    if (bfd >= 0) {
        backend_read_request(&s, bfd, req, sizeof(req));
        close(bfd);
    }
    struct got g;
    client_recv(&s, cfd, &g, 20);
    CHECK("UT-26 빈 응답 → 502, 닫힘", starts(&g, "HTTP/1.1 502 Bad Gateway\r\n") && g.eof);
    free(g.buf);
    close(cfd);
    teardown(&s, lfd);
    close(blfd);
}

/* UT-27: 중계 중에 클라이언트가 끊음 → 양쪽 정리 (S6-10) */
static void ut27_client_gone(void)
{
    struct server s;
    int lfd, bport;
    int blfd = backend_listen(&bport);
    if (blfd < 0 || set_routes(bport, 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-27 준비", 0);
        return;
    }
    int base = count_open_fds();
    int cfd = client_connect(4096);
    SEND(cfd, "GET /api/large.bin HTTP/1.1\r\nHost: x\r\n\r\n");
    int bfd = backend_accept(&s, blfd);
    char req[2048];
    if (bfd >= 0) {
        backend_read_request(&s, bfd, req, sizeof(req));
        fcntl(bfd, F_SETFL, fcntl(bfd, F_GETFL) | O_NONBLOCK);
        char chunk[16384];
        memset(chunk, 'x', sizeof(chunk));
        SEND(bfd, "HTTP/1.0 200 OK\r\n\r\n");
        for (int i = 0; i < 50; i++) {      /* 클라이언트가 안 읽으니 버퍼가 찬다 */
            if (write(bfd, chunk, sizeof(chunk)) < 0 && errno == EAGAIN)
                pump(&s, 1);
        }
    }
    pump(&s, 5);
    CHECK("UT-27 중계 중 (conn 1개, 백엔드 소켓 있음)", s.nconn == 1 && s.conns != NULL && s.conns->back_fd >= 0);
    close(cfd);                             /* 클라이언트가 떠남 */
    pump(&s, 10);
    CHECK("UT-27 nconn == 0", s.nconn == 0);
    int closed = 0;
    for (int i = 0; i < 100 && bfd >= 0 && !closed; i++) {
        char t[16384];
        ssize_t r = read(bfd, t, sizeof(t));
        if (r == 0 || (r < 0 && errno != EAGAIN))
            closed = 1;
        else if (r < 0)
            pump(&s, 1);
    }
    CHECK("UT-27 백엔드 연결도 닫힘", closed);
    if (bfd >= 0)
        close(bfd);
    pump(&s, 1);
    CHECK("UT-27 열린 fd 수가 처음과 같음", count_open_fds() == base);
    teardown(&s, lfd);
    close(blfd);
}

/* UT-28: 백엔드를 기다리는 동안 다른 연결 처리 (S6-08) */
static void ut28_not_blocked(void)
{
    struct server s;
    int lfd, bport;
    int blfd = backend_listen(&bport);
    if (blfd < 0 || set_routes(bport, 0) < 0 || setup(&s, &lfd) < 0) {
        CHECK("UT-28 준비", 0);
        return;
    }
    int c1 = client_connect(0);
    SEND(c1, "GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n");
    int bfd = backend_accept(&s, blfd);
    int c2 = client_connect(0);
    SEND(c2, "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n");
    struct got g;
    client_recv(&s, c2, &g, 10);
    CHECK("UT-28 백엔드가 응답 안 하는 동안 다른 연결은 200", starts(&g, "HTTP/1.1 200 OK\r\n"));
    free(g.buf);
    close(c1);
    close(c2);
    if (bfd >= 0)
        close(bfd);
    teardown(&s, lfd);
    close(blfd);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    alarm(60);

    printf("test_proxy\n");
    if (!make_fixture()) {
        printf("  FAIL  테스트 루트를 만들지 못함 (%s)\n", g_base);
        return 1;
    }
    ut14_to_ut16_forward();
    ut17_post_body();
    ut18_round_robin();
    ut19_static();
    ut20_to_ut21_large();
    ut22_refused();
    ut23_connect_timeout();
    ut24_to_ut25_response_timeout();
    ut26_empty();
    ut27_client_gone();
    ut28_not_blocked();
    remove_fixture();

    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
