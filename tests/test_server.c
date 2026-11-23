/*
 * 서버 단위 테스트
 * - Step 1: docs/tests/step1.md 3장 UT-01 ~ UT-06 (server_listen). 이름 앞에 S1을 붙인다.
 *   server_listen()은 포트 0으로 열어서 8080 서버와 부딪히지 않게 한다.
 * - Step 2: docs/tests/step2.md 4장 UT-40 ~ UT-48 (handle_conn). 이름 앞에 S2를 붙인다.
 *   socketpair()로 만든 소켓 두 개 중 하나를 conn에 넣어 handle_conn()에 넘기고,
 *   다른 쪽은 자식 프로세스가 클라이언트로서 요청을 쓴다.
 */
#include "server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
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

/* 지금 열려 있는 fd 개수 (/proc/self/fd 항목 수) */
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

static int get_int_opt(int fd, int level, int name)
{
    int v = -1;
    socklen_t len = sizeof(v);
    if (getsockopt(fd, level, name, &v, &len) < 0)
        return -1;
    return v;
}

/* len바이트를 다 쓸 때까지 반복 */
static int write_full(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* 상대가 끊을 때까지 읽어서 buf에 담고, 읽은 바이트 수를 돌려준다 */
static ssize_t read_until_eof(int fd, char *buf, size_t cap)
{
    size_t got = 0;
    for (;;) {
        ssize_t n = read(fd, buf + got, cap - got);
        if (n < 0)
            return -1;
        if (n == 0)
            return (ssize_t)got;
        got += (size_t)n;
        if (got == cap)
            return (ssize_t)got;
    }
}

/* UT-01 ~ UT-05: server_listen(0) */
static void ut01_to_ut05_listen(void)
{
    int fd = server_listen(0);
    CHECK("S1 UT-01 fd >= 3", fd >= 3);
    if (fd < 0)
        return;

    CHECK("S1 UT-02 LISTEN 상태 (SO_ACCEPTCONN == 1)", get_int_opt(fd, SOL_SOCKET, SO_ACCEPTCONN) == 1);
    CHECK("S1 UT-03 SO_REUSEADDR == 1", get_int_opt(fd, SOL_SOCKET, SO_REUSEADDR) == 1);

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    int ok = getsockname(fd, (struct sockaddr *)&addr, &len) == 0;
    CHECK("S1 UT-04 주소가 0.0.0.0", ok && addr.sin_addr.s_addr == htonl(INADDR_ANY));

    int port = ntohs(addr.sin_port);
    int before = count_open_fds();
    int again = server_listen(port);
    int after = count_open_fds();
    CHECK("S1 UT-05 같은 포트 다시 열면 -1", again == -1);
    CHECK("S1 UT-05 실패해도 fd가 늘지 않음", before == after);
    if (again >= 0)
        close(again);

    close(fd);
}

/* UT-06: 1024 미만 포트. root이거나 컨테이너가 낮은 포트를 허용하면 건너뜀 */
static void ut06_low_port(void)
{
    int start = -1;
    FILE *f = fopen("/proc/sys/net/ipv4/ip_unprivileged_port_start", "r");
    if (f != NULL) {
        if (fscanf(f, "%d", &start) != 1)
            start = -1;
        fclose(f);
    }
    if (geteuid() == 0 || start <= 80) {
        printf("  skip  S1 UT-06 (root이거나 ip_unprivileged_port_start=%d, FT-06의 strace로 확인)\n", start);
        return;
    }
    int fd = server_listen(80);
    CHECK("S1 UT-06 80번 포트는 -1", fd == -1);
    if (fd >= 0)
        close(fd);
}

/* ---- Step 2: handle_conn ---- */

#define SPLIT_NONE ((size_t)-1)

struct conn_result {
    int ret;            /* handle_conn() 리턴값 */
    char out[1024];     /* 클라이언트가 받은 응답 */
    ssize_t got;        /* 받은 바이트 수 */
    ssize_t leftover;   /* handle_conn() 뒤 서버 쪽 소켓에서 recv(MSG_DONTWAIT) 한 결과 */
};

static void sleep_100ms(void)
{
    struct timespec ts = {0, 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
}

/*
 * 자식은 req를 쓰고 쓰기를 닫는다. split이 SPLIT_NONE이 아니면 split바이트까지 쓰고 0.1초 쉰 뒤 나머지를 쓴다.
 * 부모는 conn을 만들어 handle_conn()을 돌리고, 서버 쪽에 남은 데이터를 확인한 뒤 응답을 끝까지 읽는다.
 */
static void run_conn(const char *req, size_t len, size_t split, struct conn_result *res)
{
    memset(res, 0, sizeof(*res));
    res->ret = -2;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return;

    pid_t writer = fork();
    if (writer == 0) {
        close(sv[1]);
        if (split != SPLIT_NONE && split < len) {
            write_full(sv[0], req, split);
            sleep_100ms();
            write_full(sv[0], req + split, len - split);
        } else {
            write_full(sv[0], req, len);
        }
        shutdown(sv[0], SHUT_WR);
        _exit(0);
    }

    struct conn *c = malloc(sizeof(*c));
    if (c == NULL)
        return;
    c->fd = sv[1];
    c->len = 0;

    res->ret = handle_conn(c);

    char t;
    res->leftover = recv(sv[1], &t, 1, MSG_DONTWAIT);
    close(sv[1]);
    free(c);

    res->got = read_until_eof(sv[0], res->out, sizeof(res->out) - 1);
    if (res->got >= 0)
        res->out[res->got] = '\0';
    close(sv[0]);
    waitpid(writer, NULL, 0);
}

/* 응답이 prefix로 시작하는지 */
static int starts_with(const struct conn_result *res, const char *prefix)
{
    size_t n = strlen(prefix);
    return res->got >= (ssize_t)n && memcmp(res->out, prefix, n) == 0;
}

/* 응답에서 s가 몇 번 나오는지 */
static int count_str(const struct conn_result *res, const char *s)
{
    int n = 0;
    for (const char *p = res->out; (p = strstr(p, s)) != NULL; p += strlen(s))
        n++;
    return n;
}

/* "GET / HTTP/1.1\r\nHost: x\r\nX: aaa…\r\n\r\n" 꼴로 헤더의 끝까지 정확히 total바이트인 요청을 만든다 */
static char *build_sized_request(size_t total)
{
    const char *head = "GET / HTTP/1.1\r\nHost: x\r\nX: ";
    const char *tail = "\r\n\r\n";
    size_t pad = total - strlen(head) - strlen(tail);
    char *req = malloc(total);
    if (req == NULL)
        return NULL;
    memcpy(req, head, strlen(head));
    memset(req + strlen(head), 'a', pad);
    memcpy(req + strlen(head) + pad, tail, strlen(tail));
    return req;
}

#define REQ(lit) (lit), (sizeof(lit) - 1)

static void ut40_ok(void)
{
    struct conn_result r;
    run_conn(REQ("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    const char *want = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    CHECK("S2 UT-40 리턴 0", r.ret == 0);
    CHECK("S2 UT-40 응답이 정확히 200 상태 줄·헤더·빈 줄",
          r.got == (ssize_t)strlen(want) && memcmp(r.out, want, strlen(want)) == 0);
}

static void ut41_split(void)
{
    const char req[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    struct conn_result r;
    run_conn(req, sizeof(req) - 1, strlen("GET / HTTP/1.1\r\n"), &r);
    CHECK("S2 UT-41 나뉘어 와도 200", starts_with(&r, "HTTP/1.1 200 OK\r\n"));
}

static void ut42_ut43_ut48_size(void)
{
    struct conn_result r;

    char *req = build_sized_request(8192);
    if (req != NULL) {
        run_conn(req, 8192, SPLIT_NONE, &r);
        CHECK("S2 UT-42 정확히 8192바이트는 200", starts_with(&r, "HTTP/1.1 200 OK\r\n"));
        free(req);
    }

    req = build_sized_request(8193);
    if (req != NULL) {
        run_conn(req, 8193, SPLIT_NONE, &r);
        CHECK("S2 UT-43 8193바이트는 431", starts_with(&r, "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
        CHECK("S2 UT-48 drain 뒤 서버 쪽에 남은 데이터 없음 (recv == 0)", r.leftover == 0);
        free(req);
    }
}

static void ut44_disconnect(void)
{
    struct conn_result r;
    run_conn(REQ("GET / HTTP/1.1\r\n"), SPLIT_NONE, &r);
    CHECK("S2 UT-44 헤더의 끝 전에 끊기면 리턴 -1", r.ret == -1);
    CHECK("S2 UT-44 아무 응답 없음", r.got == 0);
}

static void ut45_error_body(void)
{
    struct conn_result r;
    run_conn(REQ("BREW / HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S2 UT-45 상태 줄 501", starts_with(&r, "HTTP/1.1 501 Not Implemented\r\n"));
    CHECK("S2 UT-45 Content-Type: text/plain", strstr(r.out, "\r\nContent-Type: text/plain\r\n") != NULL);
    CHECK("S2 UT-45 Connection: close", strstr(r.out, "\r\nConnection: close\r\n") != NULL);

    const char *cl = strstr(r.out, "\r\nContent-Length: ");
    const char *body = strstr(r.out, "\r\n\r\n");
    long n = cl != NULL ? strtol(cl + strlen("\r\nContent-Length: "), NULL, 10) : -1;
    size_t body_len = body != NULL ? (size_t)r.got - (size_t)(body + 4 - r.out) : 0;
    CHECK("S2 UT-45 Content-Length == 본문 바이트 수", body != NULL && n == (long)body_len);
    CHECK("S2 UT-45 본문 \"501 Not Implemented\\n\"", body != NULL && strcmp(body + 4, "501 Not Implemented\n") == 0);
}

static void ut46_version(void)
{
    struct conn_result r;
    run_conn(REQ("BREW / HTTP/1.0\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S2 UT-46 요청이 1.0이어도 상태 줄은 HTTP/1.1", starts_with(&r, "HTTP/1.1 501 Not Implemented\r\n"));
}

static void ut47_one_response(void)
{
    struct conn_result r;
    run_conn(REQ("GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /next HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S2 UT-47 요청이 두 개 붙어 와도 응답은 하나", starts_with(&r, "HTTP/1.1 200 OK\r\n") && count_str(&r, "HTTP/1.1 ") == 1);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);   /* 끊긴 소켓에 써도 테스트 프로그램이 죽지 않게 */
    alarm(20);                  /* handle_conn이 멈추면 테스트를 끝냄 */

    printf("test_server\n");
    ut01_to_ut05_listen();
    ut06_low_port();
    ut40_ok();
    ut41_split();
    ut42_ut43_ut48_size();
    ut44_disconnect();
    ut45_error_body();
    ut46_version();
    ut47_one_response();
    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
