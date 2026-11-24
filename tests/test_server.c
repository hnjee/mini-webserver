/*
 * 서버 단위 테스트
 * - Step 1: docs/tests/step1.md 3장 UT-01 ~ UT-06 (server_listen). 이름 앞에 S1을 붙인다.
 *   server_listen()은 포트 0으로 열어서 8080 서버와 부딪히지 않게 한다.
 * - Step 2: docs/tests/step2.md 4장 UT-40 ~ UT-48 (handle_conn). 이름 앞에 S2를 붙인다.
 * - Step 3: docs/tests/step3.md 4장 UT-32 ~ UT-39 (handle_conn 파일 응답). 이름 앞에 S3를 붙인다.
 * - Step 4: docs/tests/step4.md 4장 UT-24 ~ UT-44 (본문, keep-alive, 남은 바이트). 이름 앞에 S4를 붙인다.
 *   Step 4부터 handle_conn은 연결을 유지하므로, 부모가 쓰기를 닫으면 다음 요청을 읽다가 끝난다.
 *   socketpair()로 만든 소켓 두 개 중 하나를 conn에 넣어 자식 프로세스에서 handle_conn()을 돌리고,
 *   부모는 다른 쪽에서 클라이언트로서 요청을 쓰고 응답을 읽는다.
 *   handle_conn()의 루트는 /tmp 아래에 만든 테스트용 www 폴더다.
 */
/* realpath()는 POSIX의 XSI 부분이라 _XOPEN_SOURCE를 켜야 선언된다 */
#define _XOPEN_SOURCE 700

#include "server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/* ---- 테스트용 루트 ---- */

#define LARGE_SIZE (1024 * 1024)
#define OUT_CAP (LARGE_SIZE + 4096)

static char g_base[] = "/tmp/ws_server_XXXXXX";
static char g_root[PATH_MAX];       /* 테스트 루트의 실제 위치 */

static const char INDEX_HTML[] = "<h1>hi</h1>\n";
static const char STYLE_CSS[] = "body { color: red; }\n";

static char pattern_byte(size_t i)
{
    return (char)((i * 31 + 7) % 251);
}

static int write_bytes(const char *dir, const char *name, const char *data, size_t len)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    int r = write_full(fd, data, len);
    close(fd);
    return r;
}

/* <base>/www 에 index.html, style.css, image.png(0~255 바이트), large.bin(1MB), secret.txt(권한 000)를 만든다 */
static int make_fixture(void)
{
    char www[PATH_MAX];
    if (mkdtemp(g_base) == NULL)
        return 0;
    snprintf(www, sizeof(www), "%s/www", g_base);
    if (mkdir(www, 0755) < 0)
        return 0;

    char png[256];
    for (int i = 0; i < 256; i++)
        png[i] = (char)i;
    char *large = malloc(LARGE_SIZE);
    if (large == NULL)
        return 0;
    for (size_t i = 0; i < LARGE_SIZE; i++)
        large[i] = pattern_byte(i);

    int ok = write_bytes(www, "index.html", INDEX_HTML, sizeof(INDEX_HTML) - 1) == 0 &&
             write_bytes(www, "style.css", STYLE_CSS, sizeof(STYLE_CSS) - 1) == 0 &&
             write_bytes(www, "image.png", png, sizeof(png)) == 0 &&
             write_bytes(www, "large.bin", large, LARGE_SIZE) == 0 &&
             write_bytes(www, "secret.txt", "secret\n", 7) == 0;
    free(large);

    char secret[PATH_MAX + 16];
    snprintf(secret, sizeof(secret), "%s/secret.txt", www);
    chmod(secret, 0);
    return ok && realpath(www, g_root) != NULL;
}

static void remove_fixture(void)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0)
        printf("  (임시 폴더 %s를 지우지 못함)\n", g_base);
}

/* ---- handle_conn 실행 ---- */

#define SPLIT_NONE ((size_t)-1)

struct conn_result {
    int ret;            /* handle_conn() 리턴값 (-2: 알 수 없음) */
    int leftover_zero;  /* handle_conn() 뒤 서버 쪽 소켓의 recv(MSG_DONTWAIT)가 0이었으면 1 */
    char *out;          /* 클라이언트가 받은 응답 (끝에 '\0'을 붙여 둠) */
    ssize_t got;        /* 받은 바이트 수 */
};

static void sleep_100ms(void)
{
    struct timespec ts = {0, 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
}

/*
 * 자식이 conn을 만들어 handle_conn(c, g_root)을 돌린다. 리턴값과 남은 데이터 여부는 종료 코드로 알린다.
 * 부모는 req를 쓰고(split이면 split바이트까지 쓰고 0.1초 쉰 뒤 나머지) 쓰기를 닫은 다음, 응답을 끝까지 읽는다.
 */
static void run_conn(const char *req, size_t len, size_t split, struct conn_result *res)
{
    res->ret = -2;
    res->leftover_zero = 0;
    res->got = -1;
    res->out = malloc(OUT_CAP + 1);
    if (res->out == NULL)
        return;
    res->out[0] = '\0';

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return;

    pid_t server = fork();
    if (server == 0) {
        close(sv[0]);
        struct conn *c = malloc(sizeof(*c));
        if (c == NULL)
            _exit(3);
        c->fd = sv[1];
        c->len = 0;
        c->body = NULL;
        c->body_len = 0;
        int r = handle_conn(c, g_root);
        char t;
        int zero = recv(sv[1], &t, 1, MSG_DONTWAIT) == 0;
        close(sv[1]);
        free(c);
        _exit((r == 0 ? 0 : r == -1 ? 1 : 2) | (zero ? 4 : 0));
    }
    close(sv[1]);

    if (split != SPLIT_NONE && split < len) {
        write_full(sv[0], req, split);
        sleep_100ms();
        write_full(sv[0], req + split, len - split);
    } else {
        write_full(sv[0], req, len);
    }
    shutdown(sv[0], SHUT_WR);

    res->got = read_until_eof(sv[0], res->out, OUT_CAP);
    if (res->got >= 0)
        res->out[res->got] = '\0';
    close(sv[0]);

    int st = 0;
    waitpid(server, &st, 0);
    if (WIFEXITED(st)) {
        int code = WEXITSTATUS(st);
        res->ret = (code & 3) == 0 ? 0 : (code & 3) == 1 ? -1 : -2;
        res->leftover_zero = (code & 4) != 0;
    }
}

static void free_result(struct conn_result *res)
{
    free(res->out);
    res->out = NULL;
}

/* 응답이 prefix로 시작하는지 */
static int starts_with(const struct conn_result *res, const char *prefix)
{
    size_t n = strlen(prefix);
    return res->got >= (ssize_t)n && memcmp(res->out, prefix, n) == 0;
}

/* 응답 헤더 영역에서 s가 몇 번 나오는지 */
static int count_str(const struct conn_result *res, const char *s)
{
    int n = 0;
    for (const char *p = res->out; (p = strstr(p, s)) != NULL; p += strlen(s))
        n++;
    return n;
}

/* 응답에서 헤더의 끝 바로 다음 위치. 없으면 -1 */
static ssize_t body_offset(const struct conn_result *res)
{
    for (ssize_t i = 3; i < res->got; i++) {
        if (memcmp(res->out + i - 3, "\r\n\r\n", 4) == 0)
            return i + 1;
    }
    return -1;
}

/* 헤더 영역에 "\r\n<line>\r\n"이 있는지 */
static int has_header_line(const struct conn_result *res, const char *line)
{
    ssize_t b = body_offset(res);
    if (b < 0)
        return 0;
    char want[256];
    snprintf(want, sizeof(want), "\r\n%s\r\n", line);
    char save = res->out[b];
    res->out[b] = '\0';
    int found = strstr(res->out, want) != NULL;
    res->out[b] = save;
    return found;
}

/* Content-Length 값. 없으면 -1 */
static long content_length(const struct conn_result *res)
{
    const char *k = "\r\nContent-Length: ";
    const char *p = strstr(res->out, k);
    return p != NULL ? strtol(p + strlen(k), NULL, 10) : -1;
}

/* 본문이 data와 바이트 단위로 같고, Content-Length도 그 길이인지 */
static int body_equals(const struct conn_result *res, const char *data, size_t len)
{
    ssize_t b = body_offset(res);
    return b >= 0 && (size_t)(res->got - b) == len && memcmp(res->out + b, data, len) == 0 &&
           content_length(res) == (long)len;
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

/* ---- Step 2: handle_conn ---- */

/* UT-40: Step 2에서는 200 본문이 없었지만(Content-Length: 0), Step 3부터 / 는 index.html을 보낸다.
   그래서 여기서는 응답 모양(상태 줄 → 헤더 → 빈 줄, 줄 끝 \r\n)만 보고, 내용은 S3 UT-33에서 본다. */
static void ut40_ok(void)
{
    struct conn_result r;
    run_conn(REQ("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S2 UT-40 리턴 0", r.ret == 0);
    CHECK("S2 UT-40 상태 줄 200", starts_with(&r, "HTTP/1.1 200 OK\r\n"));
    CHECK("S2 UT-40 Connection: keep-alive (Step 4부터 HTTP/1.1 기본 유지)", has_header_line(&r, "Connection: keep-alive"));
    CHECK("S2 UT-40 헤더의 끝 \\r\\n\\r\\n", body_offset(&r) > 0);
    free_result(&r);
}

static void ut41_split(void)
{
    const char req[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    struct conn_result r;
    run_conn(req, sizeof(req) - 1, strlen("GET / HTTP/1.1\r\n"), &r);
    CHECK("S2 UT-41 나뉘어 와도 200", starts_with(&r, "HTTP/1.1 200 OK\r\n"));
    free_result(&r);
}

static void ut42_ut43_ut48_size(void)
{
    struct conn_result r;

    char *req = build_sized_request(8192);
    if (req != NULL) {
        run_conn(req, 8192, SPLIT_NONE, &r);
        CHECK("S2 UT-42 정확히 8192바이트는 200", starts_with(&r, "HTTP/1.1 200 OK\r\n"));
        free_result(&r);
        free(req);
    }

    req = build_sized_request(8193);
    if (req != NULL) {
        run_conn(req, 8193, SPLIT_NONE, &r);
        CHECK("S2 UT-43 8193바이트는 431", starts_with(&r, "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
        CHECK("S2 UT-48 drain 뒤 서버 쪽에 남은 데이터 없음 (recv == 0)", r.leftover_zero);
        free_result(&r);
        free(req);
    }
}

static void ut44_disconnect(void)
{
    struct conn_result r;
    run_conn(REQ("GET / HTTP/1.1\r\n"), SPLIT_NONE, &r);
    CHECK("S2 UT-44 헤더의 끝 전에 끊기면 리턴 -1", r.ret == -1);
    CHECK("S2 UT-44 아무 응답 없음", r.got == 0);
    free_result(&r);
}

static void ut45_error_body(void)
{
    struct conn_result r;
    run_conn(REQ("BREW / HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S2 UT-45 상태 줄 501", starts_with(&r, "HTTP/1.1 501 Not Implemented\r\n"));
    CHECK("S2 UT-45 Content-Type: text/plain", has_header_line(&r, "Content-Type: text/plain"));
    CHECK("S2 UT-45 Connection: close", has_header_line(&r, "Connection: close"));
    CHECK("S2 UT-45 본문 \"501 Not Implemented\\n\", Content-Length == 본문 바이트 수",
          body_equals(&r, REQ("501 Not Implemented\n")));
    free_result(&r);
}

static void ut46_version(void)
{
    struct conn_result r;
    run_conn(REQ("BREW / HTTP/1.0\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S2 UT-46 요청이 1.0이어도 상태 줄은 HTTP/1.1", starts_with(&r, "HTTP/1.1 501 Not Implemented\r\n"));
    free_result(&r);
}

static void ut47_one_response(void)
{
    struct conn_result r;
    run_conn(REQ("GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /next HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S2 UT-47 요청이 두 개 붙어 오면 응답도 둘 (Step 4부터 남은 바이트를 다음 요청으로 씀)",
          starts_with(&r, "HTTP/1.1 200 OK\r\n") && count_str(&r, "HTTP/1.1 ") == 2);
    free_result(&r);
}

/* ---- Step 3: handle_conn 파일 응답 ---- */

static void ut32_style(void)
{
    struct conn_result r;
    run_conn(REQ("GET /style.css HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S3 UT-32 리턴 0", r.ret == 0);
    CHECK("S3 UT-32 상태 줄 200", starts_with(&r, "HTTP/1.1 200 OK\r\n"));
    CHECK("S3 UT-32 Content-Type: text/css; charset=utf-8", has_header_line(&r, "Content-Type: text/css; charset=utf-8"));
    CHECK("S3 UT-32 Connection: keep-alive", has_header_line(&r, "Connection: keep-alive"));
    CHECK("S3 UT-32 본문 == 파일, Content-Length == 파일 크기", body_equals(&r, REQ(STYLE_CSS)));
    free_result(&r);
}

static void ut33_index(void)
{
    struct conn_result r;
    run_conn(REQ("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    char want[256];
    int n = snprintf(want, sizeof(want),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n"
                     "%s",
                     sizeof(INDEX_HTML) - 1, INDEX_HTML);
    CHECK("S3 UT-33 / 는 index.html (응답 전체가 정확히 일치)",
          r.got == n && memcmp(r.out, want, (size_t)n) == 0);
    free_result(&r);
}

static void ut34_binary(void)
{
    char png[256];
    for (int i = 0; i < 256; i++)
        png[i] = (char)i;
    struct conn_result r;
    run_conn(REQ("GET /image.png HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S3 UT-34 Content-Type: image/png", has_header_line(&r, "Content-Type: image/png"));
    CHECK("S3 UT-34 0~255 바이트가 그대로 (\\0 포함)", body_equals(&r, png, sizeof(png)));
    free_result(&r);
}

static void ut35_large(void)
{
    char *large = malloc(LARGE_SIZE);
    if (large == NULL)
        return;
    for (size_t i = 0; i < LARGE_SIZE; i++)
        large[i] = pattern_byte(i);
    struct conn_result r;
    run_conn(REQ("GET /large.bin HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S3 UT-35 Content-Type: application/octet-stream", has_header_line(&r, "Content-Type: application/octet-stream"));
    CHECK("S3 UT-35 1MB가 그대로", body_equals(&r, large, LARGE_SIZE));
    free_result(&r);
    free(large);
}

static void ut36_not_found(void)
{
    struct conn_result r;
    run_conn(REQ("GET /nope.html HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S3 UT-36 상태 줄 404", starts_with(&r, "HTTP/1.1 404 Not Found\r\n"));
    CHECK("S3 UT-36 본문 \"404 Not Found\\n\"", body_equals(&r, REQ("404 Not Found\n")));
    free_result(&r);
}

static void ut37_forbidden(void)
{
    if (geteuid() == 0) {
        printf("  skip  S3 UT-37 (root는 권한 000 파일도 읽을 수 있음)\n");
        return;
    }
    struct conn_result r;
    run_conn(REQ("GET /secret.txt HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S3 UT-37 읽기 권한 없는 파일은 403", starts_with(&r, "HTTP/1.1 403 Forbidden\r\n"));
    CHECK("S3 UT-37 본문 \"403 Forbidden\\n\"", body_equals(&r, REQ("403 Forbidden\n")));
    free_result(&r);
}

static void ut38_traversal(void)
{
    struct conn_result r;
    run_conn(REQ("GET /../../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S3 UT-38 /../../etc/passwd 는 403", starts_with(&r, "HTTP/1.1 403 Forbidden\r\n"));
    free_result(&r);
}

static void ut39_query(void)
{
    struct conn_result r;
    run_conn(REQ("GET /style.css?v=3 HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S3 UT-39 ?v=3 이 붙어도 style.css", starts_with(&r, "HTTP/1.1 200 OK\r\n") && body_equals(&r, REQ(STYLE_CSS)));
    free_result(&r);
}

/* ---- Step 4: 본문, keep-alive, 남은 바이트 ---- */

/* 응답에서 a가 나온 뒤에 b가 나오면 1 */
static int in_order(const struct conn_result *res, const char *a, const char *b)
{
    const char *p = strstr(res->out, a);
    return p != NULL && strstr(p + strlen(a), b) != NULL;
}

/* 응답 개수 (상태 줄 "HTTP/1.1 " 수). 테스트 본문에는 이 글자가 없다 */
static int responses(const struct conn_result *res)
{
    return count_str(res, "HTTP/1.1 ");
}

#define GET_STYLE "GET /style.css HTTP/1.1\r\nHost: x\r\n\r\n"

static void s4_ut24_to_ut28_echo(void)
{
    struct conn_result r;

    run_conn(REQ("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: 11\r\n\r\nname=hyunji"), SPLIT_NONE, &r);
    CHECK("S4 UT-24 리턴 0", r.ret == 0);
    CHECK("S4 UT-24 200", starts_with(&r, "HTTP/1.1 200 OK\r\n"));
    CHECK("S4 UT-24 Content-Type을 요청에서 따름",
          has_header_line(&r, "Content-Type: application/x-www-form-urlencoded"));
    CHECK("S4 UT-24 Connection: keep-alive", has_header_line(&r, "Connection: keep-alive"));
    CHECK("S4 UT-24 본문 name=hyunji, Content-Length 11", body_equals(&r, REQ("name=hyunji")));
    free_result(&r);

    run_conn(REQ("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc"), SPLIT_NONE, &r);
    CHECK("S4 UT-25 Content-Type 없으면 application/octet-stream",
          has_header_line(&r, "Content-Type: application/octet-stream") && body_equals(&r, REQ("abc")));
    free_result(&r);

    const char split[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\nname=hyunji";
    run_conn(split, sizeof(split) - 1, sizeof(split) - 1 - 6, &r);
    CHECK("S4 UT-26 본문이 나뉘어 와도 다 모음", body_equals(&r, REQ("name=hyunji")));
    free_result(&r);

    run_conn(REQ("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S4 UT-27 Content-Length 0 → 200, 본문 없음",
          starts_with(&r, "HTTP/1.1 200 OK\r\n") && body_equals(&r, "", 0));
    free_result(&r);

    char req[512];
    int n = snprintf(req, sizeof(req), "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 256\r\n\r\n");
    for (int i = 0; i < 256; i++)
        req[n + i] = (char)i;
    char bin[256];
    for (int i = 0; i < 256; i++)
        bin[i] = (char)i;
    run_conn(req, (size_t)n + 256, SPLIT_NONE, &r);
    CHECK("S4 UT-28 0~255 바이트 본문이 그대로 (\\0 포함)", body_equals(&r, bin, sizeof(bin)));
    free_result(&r);
}

static void s4_ut29_to_ut34_keep_alive(void)
{
    struct conn_result r;

    run_conn(REQ(GET_STYLE "GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S4 UT-29 요청 두 개 → 응답 두 개", responses(&r) == 2);
    CHECK("S4 UT-29 첫째 keep-alive, 둘째 close",
          in_order(&r, "Connection: keep-alive\r\n", "Connection: close\r\n"));
    CHECK("S4 UT-29 순서: style.css 뒤에 index.html", in_order(&r, STYLE_CSS, INDEX_HTML));
    free_result(&r);

    run_conn(REQ("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello" GET_STYLE), SPLIT_NONE, &r);
    CHECK("S4 UT-30 본문 뒤 바이트를 다음 요청으로", responses(&r) == 2 && in_order(&r, "hello", STYLE_CSS));
    free_result(&r);

    run_conn(REQ("GET /style.css HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
                 "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S4 UT-31 GET의 본문은 읽고 버림 → 응답 두 개", responses(&r) == 2 && in_order(&r, STYLE_CSS, INDEX_HTML));
    free_result(&r);

    run_conn(REQ("GET /nope.html HTTP/1.1\r\nHost: x\r\n\r\n" GET_STYLE), SPLIT_NONE, &r);
    CHECK("S4 UT-32 404 뒤에도 연결 유지 → 200", responses(&r) == 2 &&
          in_order(&r, "HTTP/1.1 404 Not Found\r\n", "HTTP/1.1 200 OK\r\n"));
    free_result(&r);

    run_conn(REQ("GET / HTTP/1.0\r\n\r\n" GET_STYLE), SPLIT_NONE, &r);
    CHECK("S4 UT-33 HTTP/1.0 → 응답 하나, Connection: close",
          responses(&r) == 1 && has_header_line(&r, "Connection: close"));
    free_result(&r);

    run_conn(REQ("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n" GET_STYLE), SPLIT_NONE, &r);
    CHECK("S4 UT-34 HTTP/1.0 keep-alive → 응답 둘, 첫째 keep-alive",
          responses(&r) == 2 && has_header_line(&r, "Connection: keep-alive"));
    free_result(&r);
}

static void s4_ut35_to_ut44_errors(void)
{
    struct conn_result r;

    run_conn(REQ("POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na=1" GET_STYLE), SPLIT_NONE, &r);
    CHECK("S4 UT-35 POST 다른 경로 → 405, Allow: GET",
          starts_with(&r, "HTTP/1.1 405 Method Not Allowed\r\n") && has_header_line(&r, "Allow: GET"));
    CHECK("S4 UT-35 405 뒤에도 연결 유지 → 200", responses(&r) == 2 && in_order(&r, "405", "HTTP/1.1 200 OK"));
    free_result(&r);

    run_conn(REQ("GET /echo HTTP/1.1\r\nHost: x\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S4 UT-36 GET /echo → 405, Allow: POST",
          starts_with(&r, "HTTP/1.1 405 Method Not Allowed\r\n") && has_header_line(&r, "Allow: POST"));
    free_result(&r);

    run_conn(REQ("POST /echo HTTP/1.1\r\nHost: x\r\n\r\n" GET_STYLE), SPLIT_NONE, &r);
    CHECK("S4 UT-37 Content-Length 없음 → 411, 닫음",
          starts_with(&r, "HTTP/1.1 411 Length Required\r\n") && has_header_line(&r, "Connection: close") &&
          responses(&r) == 1);
    free_result(&r);

    run_conn(REQ("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S4 UT-38 Content-Length abc → 400, 닫음",
          starts_with(&r, "HTTP/1.1 400 Bad Request\r\n") && has_header_line(&r, "Connection: close"));
    free_result(&r);

    run_conn(REQ("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 7\r\n\r\nhello"), SPLIT_NONE, &r);
    CHECK("S4 UT-39 Content-Length 5와 7 → 400, 닫음",
          starts_with(&r, "HTTP/1.1 400 Bad Request\r\n") && has_header_line(&r, "Connection: close"));
    free_result(&r);

    run_conn(REQ("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S4 UT-40 Transfer-Encoding → 501, 닫음",
          starts_with(&r, "HTTP/1.1 501 Not Implemented\r\n") && has_header_line(&r, "Connection: close"));
    free_result(&r);

    run_conn(REQ("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n"), SPLIT_NONE, &r);
    CHECK("S4 UT-41 1048577바이트 → 413, 닫음",
          starts_with(&r, "HTTP/1.1 413 Content Too Large\r\n") && has_header_line(&r, "Connection: close"));
    free_result(&r);

    char *big = malloc(LARGE_SIZE + 128);
    if (big != NULL) {
        int n = snprintf(big, 128, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", LARGE_SIZE);
        for (size_t i = 0; i < LARGE_SIZE; i++)
            big[(size_t)n + i] = pattern_byte(i);
        run_conn(big, (size_t)n + LARGE_SIZE, SPLIT_NONE, &r);
        CHECK("S4 UT-42 정확히 1MB → 200, 그대로", starts_with(&r, "HTTP/1.1 200 OK\r\n") &&
              body_equals(&r, big + n, LARGE_SIZE));
        free_result(&r);
        free(big);
    }

    char *uri = malloc(8192);
    if (uri != NULL) {
        size_t n = (size_t)snprintf(uri, 8192, "GET /");
        memset(uri + n, 'a', 4096);
        n += 4096;
        n += (size_t)snprintf(uri + n, 8192 - n, " HTTP/1.1\r\nHost: x\r\n\r\n");
        run_conn(uri, n, SPLIT_NONE, &r);
        CHECK("S4 UT-43 경로 4097바이트 → 414, 닫음",
              starts_with(&r, "HTTP/1.1 414 URI Too Long\r\n") && has_header_line(&r, "Connection: close"));
        free_result(&r);
        free(uri);
    }

    run_conn(REQ("GET /\r\nHost: x\r\n\r\n" GET_STYLE), SPLIT_NONE, &r);
    CHECK("S4 UT-44 요청 줄이 틀리면 400, 닫음, 응답 하나",
          starts_with(&r, "HTTP/1.1 400 Bad Request\r\n") && has_header_line(&r, "Connection: close") &&
          responses(&r) == 1);
    free_result(&r);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);   /* 끊긴 소켓에 써도 테스트 프로그램이 죽지 않게 */
    alarm(60);                  /* handle_conn이 멈추면 테스트를 끝냄 */

    printf("test_server\n");
    ut01_to_ut05_listen();
    ut06_low_port();

    if (!make_fixture()) {
        printf("  FAIL  테스트 루트를 만들지 못함 (%s)\n", g_base);
        return 1;
    }
    ut40_ok();
    ut41_split();
    ut42_ut43_ut48_size();
    ut44_disconnect();
    ut45_error_body();
    ut46_version();
    ut47_one_response();

    ut32_style();
    ut33_index();
    ut34_binary();
    ut35_large();
    ut36_not_found();
    ut37_forbidden();
    ut38_traversal();
    ut39_query();

    s4_ut24_to_ut28_echo();
    s4_ut29_to_ut34_keep_alive();
    s4_ut35_to_ut44_errors();
    remove_fixture();

    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
