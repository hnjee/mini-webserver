/* Step 2·3 단위 테스트: http_conn_serve를 socketpair와 임시 루트로 시험한다(TC-2-07, D-19 보조, TC-3-08, TC-3-10). */
#define _XOPEN_SOURCE 700 /* mkdtemp·realpath */

#include "../src/http_conn.h"
#include "../src/http_parser.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static char tmp[PATH_MAX];  /* mkdtemp 결과(정규화) */
static char root[PATH_MAX]; /* main처럼 realpath로 정규화한 임시 루트 */

/* 임시 루트의 파일 내용 */
static const char index_body[] = "<!DOCTYPE html><h1>index</h1>\n";
#define BIG_LEN (3 * 65536 + 123) /* 64KB 조각을 여러 번 넘는다 */
#define HUGE_LEN (4 * 1024 * 1024) /* 소켓 버퍼보다 커서 도중에 닫으면 EPIPE가 난다 */
static char *big_body;

static void check(const char *name, int condition, const char *detail)
{
    if (condition) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (%s)\n", name, detail);
        failures++;
    }
}

static void setup_or_die(int ok, const char *what)
{
    if (!ok) {
        printf("  SETUP ERROR %s: %s\n", what, strerror(errno));
        exit(2);
    }
}

/* 서버가 먼저 닫아 생기는 쓰기 실패는 제품 실패로 기록해야 하므로 실패를 돌려준다. */
static int try_write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n <= 0) {
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static void write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        setup_or_die(n > 0, "테스트 write");
        data += n;
        len -= (size_t)n;
    }
}

/* EOF까지 읽는다. 읽은 길이를 돌려준다. 끝에 NUL을 붙인다. */
static size_t read_all(int fd, char *out, size_t cap)
{
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, out + total, cap - 1 - total);
        if (n < 0 && errno == ECONNRESET) {
            n = 0; /* 서버가 입력을 다 읽지 않고 닫으면 생긴다. 응답 비교에서 실패로 잡힌다 */
        }
        setup_or_die(n >= 0, "테스트 read");
        if (n == 0) {
            out[total] = '\0';
            return total;
        }
        total += (size_t)n;
        setup_or_die(total < cap - 1, "응답이 테스트 버퍼보다 크다");
    }
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* 응답 머리 뒤 본문이 body와 바이트 단위로 같고 Content-Length도 그 길이인가. */
static int body_is(const char *reply, size_t reply_len, const char *body, size_t body_len)
{
    const char *end = strstr(reply, "\r\n\r\n");
    char cl[64];
    size_t head_len;

    if (end == NULL) {
        return 0;
    }
    head_len = (size_t)(end - reply) + 4;
    snprintf(cl, sizeof(cl), "\r\nContent-Length: %zu\r\n", body_len);
    return reply_len - head_len == body_len && memcmp(reply + head_len, body, body_len) == 0 &&
           strstr(reply, cl) != NULL && strstr(reply, cl) < end;
}

/* 가장 낮은 빈 fd 번호. 호출 전후가 같으면 새로 열린 fd가 남지 않았다(TC-3-08). */
static int lowest_free_fd(void)
{
    int fd = open("/dev/null", O_RDONLY);
    setup_or_die(fd >= 0, "open /dev/null");
    close(fd);
    return fd;
}

/* base + "/" + rel. 넘치면 준비 실패. */
static void path_in(char *out, const char *base, const char *rel)
{
    int n = snprintf(out, PATH_MAX, "%s/%s", base, rel);
    setup_or_die(n > 0 && n < PATH_MAX, "경로 조립");
}

static void make_file(const char *rel, const char *data, size_t len)
{
    char p[PATH_MAX];
    int fd;

    path_in(p, root, rel);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    setup_or_die(fd >= 0, p);
    setup_or_die(write(fd, data, len) == (ssize_t)len, "write");
    close(fd);
}

/* 임시 루트: index.html, big.bin(BIG_LEN), huge.bin(HUGE_LEN), sub/, loop -> loop */
static void build_root(void)
{
    char p[PATH_MAX];
    char *huge = malloc(HUGE_LEN);
    size_t i;

    big_body = malloc(BIG_LEN);
    setup_or_die(big_body != NULL && huge != NULL, "malloc");
    setup_or_die(mkdtemp(strcpy(p, "/tmp/test_http_conn-XXXXXX")) != NULL, "mkdtemp");
    setup_or_die(realpath(p, tmp) != NULL, "realpath");
    path_in(root, tmp, "www");
    setup_or_die(mkdir(root, 0755) == 0, "mkdir www");
    make_file("index.html", index_body, sizeof(index_body) - 1);
    for (i = 0; i < BIG_LEN; i++) {
        big_body[i] = (char)(i * 131 + 17);
    }
    make_file("big.bin", big_body, BIG_LEN);
    memset(huge, 'h', HUGE_LEN);
    make_file("huge.bin", huge, HUGE_LEN);
    free(huge);
    path_in(p, root, "sub");
    setup_or_die(mkdir(p, 0755) == 0, "mkdir sub");
    path_in(p, root, "loop");
    setup_or_die(symlink("loop", p) == 0, "symlink loop");
}

static void remove_root(void)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp); /* tmp는 mkdtemp가 만든 따옴표 없는 경로다 */
    if (system(cmd) != 0) {
        printf("  (임시 폴더 정리 실패: %s)\n", tmp);
    }
    free(big_body);
}

/*
 * 요청 전체를 미리 써 두고(쓰기 방향은 열어 둔다) http_conn_serve를 부른 뒤, server를 닫고 응답을 읽는다.
 * 쓰기 방향을 열어 두므로 서버가 요청 머리 뒤에 더 읽으려 하면 막혀 alarm으로 실패한다.
 */
static http_conn_result serve_prewritten(const char *req, size_t len, char *reply, size_t cap)
{
    int fds[2];
    http_conn_result r;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], req, len);
    r = http_conn_serve(fds[1], root);
    close(fds[1]);
    read_all(fds[0], reply, cap);
    close(fds[0]);
    return r;
}

/* TC-2-07: 다른 프로세스가 서버 역할을 하고, 이 프로세스가 요청을 두 번에 나눠 쓴다. */
static void test_split_request(void)
{
    static char reply[4096];
    const struct timespec gap = {0, 200 * 1000 * 1000};
    int fds[2], status, wrote;
    size_t reply_len;
    pid_t pid;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        http_conn_result r;
        close(fds[0]);
        r = http_conn_serve(fds[1], root);
        close(fds[1]);
        _exit(r == HTTP_CONN_RESPONDED ? 0 : r == HTTP_CONN_PEER_CLOSED ? 1 : 2);
    }
    close(fds[1]);
    wrote = try_write_all(fds[0], "GET / HTTP/1.1\r\n", 16) == 0;
    nanosleep(&gap, NULL);
    wrote = wrote && try_write_all(fds[0], "Host: x\r\n\r\n", 11) == 0;
    check("TC-2-07 두 조각 모두 쓰기 성공", wrote, "서버가 요청 도중 닫았다");
    reply_len = read_all(fds[0], reply, sizeof(reply));
    close(fds[0]);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    check("TC-2-07 나눠 쓴 요청에 200", starts_with(reply, "HTTP/1.1 200 OK\r\n"), "200 응답이 아니다");
    check("TC-2-07 나눠 쓴 요청 본문 = index.html", body_is(reply, reply_len, index_body, sizeof(index_body) - 1),
          "본문이 다르다");
    check("TC-2-07 나눠 쓴 요청 RESPONDED", WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "RESPONDED가 아니다");
}

/* TC-2-07: 머리 도중 상대가 쓰기를 닫으면 응답 없이 PEER_CLOSED. */
static void test_close_mid_head(void)
{
    static char reply[4096];
    int fds[2];
    http_conn_result r;
    size_t n;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], "GET / HTTP/1.1\r\nHo", 18);
    setup_or_die(shutdown(fds[0], SHUT_WR) == 0, "shutdown");
    r = http_conn_serve(fds[1], root);
    close(fds[1]);
    n = read_all(fds[0], reply, sizeof(reply));
    close(fds[0]);
    check("TC-2-07 머리 도중 닫기 PEER_CLOSED", r == HTTP_CONN_PEER_CLOSED, "PEER_CLOSED가 아니다");
    check("TC-2-07 머리 도중 닫기 응답 없음", n == 0, "응답 바이트가 있다");
}

/*
 * D-19 보조: 결과별 상태 코드, 8192바이트로 버퍼가 꽉 찬 경우(0바이트 read 없이 400).
 * D-30 3단계: static_file 결과별 상태 코드. 본문과 Content-Length도 비교한다.
 */
static void test_status_mapping(void)
{
    static const struct {
        const char *name;
        const char *req;
        const char *line;
        const char *body;
    } cases[] = {
        {"D-19 OK → 200 index.html", "GET / HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 200 OK\r\n", index_body},
        {"D-19 POST 본문은 읽지 않고 200 index.html (D-37)",
         "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc", "HTTP/1.1 200 OK\r\n", index_body},
        {"TC-3-10 POST /index.html 본문 있음 → GET과 같은 200 (D-37)",
         "POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\nname=hyunji", "HTTP/1.1 200 OK\r\n",
         index_body},
        {"D-30 REDIRECT → 301", "GET /sub HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 301 Moved Permanently\r\n",
         "301 Moved Permanently\n"},
        {"D-30 BAD_TARGET → 400", "GET index.html HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 400 Bad Request\r\n",
         "400 Bad Request\n"},
        {"D-30 FORBIDDEN → 403", "GET /../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 403 Forbidden\r\n",
         "403 Forbidden\n"},
        {"D-30 NOT_FOUND → 404", "GET /nope.html HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n",
         "404 Not Found\n"},
        {"D-30 ERROR → 500 (ELOOP)", "GET /loop HTTP/1.1\r\nHost: x\r\n\r\n",
         "HTTP/1.1 500 Internal Server Error\r\n", "500 Internal Server Error\n"},
        {"D-44 HEAD / HTTP/1.1 → 501 본문 그대로", "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n",
         "HTTP/1.1 501 Not Implemented\r\n", "501 Not Implemented\n"},
        {"D-19 BAD_REQUEST → 400", "GET / HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request\r\n", "400 Bad Request\n"},
        {"D-19 NOT_IMPLEMENTED → 501", "BREW / HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 501 Not Implemented\r\n",
         "501 Not Implemented\n"},
        {"D-19 VERSION_NOT_SUPPORTED → 505", "GET / HTTP/2.0\r\nHost: x\r\n\r\n",
         "HTTP/1.1 505 HTTP Version Not Supported\r\n", "505 HTTP Version Not Supported\n"},
    };
    static char reply[4096], full[HTTP_REQUEST_HEAD_MAX];
    size_t i, reply_len;
    http_conn_result r;
    char label[160];

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int fds[2];
        setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
        write_all(fds[0], cases[i].req, strlen(cases[i].req));
        r = http_conn_serve(fds[1], root);
        close(fds[1]);
        reply_len = read_all(fds[0], reply, sizeof(reply));
        close(fds[0]);
        check(cases[i].name, r == HTTP_CONN_RESPONDED && starts_with(reply, cases[i].line), reply);
        snprintf(label, sizeof(label), "%s 본문·Content-Length", cases[i].name);
        check(label, body_is(reply, reply_len, cases[i].body, strlen(cases[i].body)), "본문이 다르다");
    }
    {
        const char req[] = "GET //sub?v=1 HTTP/1.1\r\nHost: x\r\n\r\n";
        r = serve_prewritten(req, sizeof(req) - 1, reply, sizeof(reply));
    }
    check("D-30 301 Location: /sub/ (C1, D-42)",
          r == HTTP_CONN_RESPONDED && strstr(reply, "\r\nLocation: /sub/\r\nConnection: close\r\n") != NULL, reply);

    memset(full, 'a', sizeof(full));
    r = serve_prewritten(full, sizeof(full), reply, sizeof(reply));
    check("D-19 종료 없이 8192바이트 → 400 (0바이트 read 없음)",
          r == HTTP_CONN_RESPONDED && starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"),
          r == HTTP_CONN_PEER_CLOSED ? "PEER_CLOSED로 끝났다" : "400 응답이 아니다");
}

/* D-19·D-33: 파일 응답의 머리 쓰기 실패. 파일은 열렸으므로 fd 누수도 본다(TC-3-08). */
static void test_write_failure(void)
{
    const char req[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    int fds[2], err, before;
    http_conn_result r;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], req, sizeof(req) - 1);
    close(fds[0]); /* 요청은 받을 수 있지만 응답 쓰기는 EPIPE가 된다 */
    before = lowest_free_fd();
    errno = 0;
    r = http_conn_serve(fds[1], root);
    err = errno;
    check("TC-3-08 머리 쓰기 실패 뒤 파일 fd 누수 없음", lowest_free_fd() == before, "열린 fd가 남았다");
    close(fds[1]);
    check("D-19 응답(파일 머리) 쓰기 실패 ERROR, errno EPIPE", r == HTTP_CONN_ERROR && err == EPIPE, strerror(err));
}

/* TC-3-10: 64KB를 여러 번 넘는 파일을 다른 프로세스가 서버로 보내고 이 프로세스가 읽어 바이트 비교. */
static void test_big_file(void)
{
    const char req[] = "GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n";
    const size_t cap = BIG_LEN + 4096;
    char *reply = malloc(cap);
    size_t reply_len;
    int fds[2], status;
    pid_t pid;

    setup_or_die(reply != NULL, "malloc");
    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], req, sizeof(req) - 1);
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        http_conn_result r;
        close(fds[0]);
        r = http_conn_serve(fds[1], root);
        close(fds[1]);
        _exit(r == HTTP_CONN_RESPONDED ? 0 : 1);
    }
    close(fds[1]);
    reply_len = read_all(fds[0], reply, cap);
    close(fds[0]);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    check("TC-3-10 3×64KB+123 파일 RESPONDED", WIFEXITED(status) && WEXITSTATUS(status) == 0, "자식 실패");
    check("TC-3-10 3×64KB+123 파일 200", starts_with(reply, "HTTP/1.1 200 OK\r\n"), "200이 아니다");
    check("TC-3-10 3×64KB+123 파일 바이트·Content-Length 일치", body_is(reply, reply_len, big_body, BIG_LEN),
          "본문이 다르다");
    check("TC-3-10 .bin Content-Type octet-stream",
          strstr(reply, "\r\nContent-Type: application/octet-stream\r\n") != NULL, "다르다");
    free(reply);
}

/* TC-3-10·TC-3-08: 본문 도중 읽는 쪽이 닫으면 ERROR·EPIPE, 파일 fd 누수 없음(자식 안에서 확인). */
static void test_close_mid_body(void)
{
    const char req[] = "GET /huge.bin HTTP/1.1\r\nHost: x\r\n\r\n";
    char buf[1024];
    int fds[2], status;
    pid_t pid;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], req, sizeof(req) - 1);
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        http_conn_result r;
        int before, err, leaked;
        close(fds[0]);
        before = lowest_free_fd();
        errno = 0;
        r = http_conn_serve(fds[1], root);
        err = errno;
        leaked = lowest_free_fd() != before;
        close(fds[1]);
        _exit(leaked ? 3 : r != HTTP_CONN_ERROR ? 1 : err != EPIPE ? 2 : 0);
    }
    close(fds[1]);
    setup_or_die(read(fds[0], buf, sizeof(buf)) > 0, "응답 앞부분 read");
    close(fds[0]); /* 4MB 중 앞부분만 읽고 닫는다 */
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    check("TC-3-10 본문 도중 닫기 ERROR, errno EPIPE", WIFEXITED(status) && WEXITSTATUS(status) != 1 &&
                                                       WEXITSTATUS(status) != 2,
          "ERROR·EPIPE가 아니다");
    check("TC-3-08 본문 도중 닫기 뒤 파일 fd 누수 없음", WIFEXITED(status) && WEXITSTATUS(status) != 3,
          "열린 fd가 남았다");
}

int main(void)
{
    alarm(10); /* 서버가 멈추면 끝없이 기다리지 않게 한다 */
    signal(SIGPIPE, SIG_IGN); /* D-05: 서버와 같은 조건 */
    printf("test_http_conn (TC-2-07, TC-3-08, TC-3-10)\n");
    build_root();
    test_split_request();
    test_close_mid_head();
    test_status_mapping();
    test_write_failure();
    test_big_file();
    test_close_mid_body();
    remove_root();

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
