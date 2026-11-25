/*
 * Step 2~5 단위 테스트: http_conn 상태 구조체를 socketpair와 임시 루트로 시험한다
 * (TC-2-07, D-17 보조, TC-3-08, TC-3-10, TC-4-10·19·20·22·24~36, TC-5-07·09·10).
 * Step 4의 http_conn_serve 자리는 시험 쪽 poll 구동 함수 drive()가 맡는다(D-57). 루프와 같은 규칙으로
 * 준비되면 한 차례 실행, RUNNABLE이면 기다리지 않고 다시 실행, 실행 뒤 기다리는 중이면 마감을 확인한다.
 * 서버는 연결을 유지하므로(D-51) 요청을 다 쓴 뒤 쓰기 방향을 닫아야 돌아온다. 5초를 기다리는 시험은 맨 끝에 자식 프로세스로 함께 돌린다.
 * TC-5-09·10의 직접 호출 시험은 now를 시험이 정해 넘기고(가짜 시계), 부분 쓰기는 out_pos·out_len 필드로 본다(임시 관찰 지점).
 */
#define _XOPEN_SOURCE 700 /* mkdtemp·realpath */

#include "../src/http_conn.h"
#include "../src/http_parser.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
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

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    setup_or_die(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "O_NONBLOCK");
}

/*
 * D-57: 시험 쪽 poll 구동 함수. 연결 하나를 끝날 때까지 돌리고 결과를 돌려준다. errno에는 기록된 오류 번호를 둔다.
 * 루프와 같은 규칙: 준비되면(HUP·ERR 포함) 실행 → RUNNABLE이면 기다리지 않고 다시 실행 → 기다리는 중이면 마감 확인.
 * 돌아가기 전에 http_conn_release로 파일 fd를 닫는다. 소켓은 닫지 않는다.
 */
static http_conn_result drive(int fd, const char *root_dir)
{
    http_conn *c = malloc(sizeof(*c));
    http_conn_step step = HTTP_CONN_WANT_READ;
    http_conn_result r;
    int err;

    setup_or_die(c != NULL, "malloc");
    set_nonblocking(fd);
    http_conn_init(c, fd, root_dir, NULL, now_ns());
    for (;;) {
        struct pollfd pfd;
        long long left = c->deadline - now_ns();
        int n;

        pfd.fd = fd;
        pfd.events = step == HTTP_CONN_WANT_WRITE ? POLLOUT : POLLIN;
        pfd.revents = 0;
        n = poll(&pfd, 1, left <= 0 ? 0 : (int)((left + 999999) / 1000000));
        setup_or_die(n >= 0 || errno == EINTR, "poll");
        if (n > 0) {
            do {
                step = http_conn_run(c, HTTP_CONN_WAKE_CLIENT, now_ns());
            } while (step == HTTP_CONN_RUNNABLE);
            if (step == HTTP_CONN_FINISHED) {
                break;
            }
        }
        if (http_conn_expired(c, now_ns())) {
            http_conn_expire(c);
            break;
        }
    }
    r = c->result;
    err = c->err;
    http_conn_release(c);
    free(c);
    errno = err;
    return r;
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

/* 임시 루트: index.html, big.bin(BIG_LEN), huge.bin(HUGE_LEN), echo(파일), sub/, loop -> loop */
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
    make_file("echo", "echo-file\n", 10); /* TC-4-25: /echo가 파일보다 먼저다 */
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
 * 요청 전체를 미리 써 두고 쓰기 방향을 닫은 뒤 http_conn_serve를 부르고, server를 닫고 응답을 읽는다.
 * 유지 응답 뒤 서버는 다음 요청을 읽다가 EOF를 만나 돌아온다(D-53 요청 사이). reply_len에 응답 길이를 쓴다.
 */
static http_conn_result serve_prewritten_len(const char *req, size_t len, char *reply, size_t cap, size_t *reply_len)
{
    int fds[2];
    http_conn_result r;
    size_t n;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], req, len);
    setup_or_die(shutdown(fds[0], SHUT_WR) == 0, "shutdown");
    r = drive(fds[1], root);
    close(fds[1]);
    n = read_all(fds[0], reply, cap);
    close(fds[0]);
    if (reply_len != NULL) {
        *reply_len = n;
    }
    return r;
}

static http_conn_result serve_prewritten(const char *req, size_t len, char *reply, size_t cap)
{
    return serve_prewritten_len(req, len, reply, cap, NULL);
}

#define NPOS ((size_t)-1)

static size_t find_in(const char *hay, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    for (i = 0; i + n <= len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return i;
        }
    }
    return NPOS;
}

/* p[0..len)의 첫 응답 길이(머리 + Content-Length). 100은 머리만. 머리나 본문이 모자라면 0. */
static size_t response_len(const char *p, size_t len)
{
    size_t head = find_in(p, len, "\r\n\r\n"), at;
    unsigned long cl;

    if (head == NPOS) {
        return 0;
    }
    head += 4;
    if (len >= 13 && memcmp(p, "HTTP/1.1 100 ", 13) == 0) {
        return head;
    }
    at = find_in(p, head, "\r\nContent-Length: ");
    if (at == NPOS) {
        return 0;
    }
    cl = strtoul(p + at + 18, NULL, 10);
    return head + cl <= len ? head + cl : 0;
}

/* 기대 응답 하나: 상태 줄, 본문, Connection 값(NULL이면 Connection 머리 없음), 머리에 꼭 있어야 할 줄(NULL 가능). */
typedef struct {
    const char *line;
    const char *body;
    size_t body_len;
    const char *conn;
    const char *must;
} want_resp;

/* reply가 want[0..n)과 순서대로 정확히 같은 응답들이고 뒤에 남는 바이트가 없는가. */
static void check_replies(const char *name, const char *reply, size_t reply_len, const want_resp *want, size_t n)
{
    char label[256], conn[64];
    size_t i, at = 0;

    for (i = 0; i < n; i++) {
        const char *p = reply + at;
        size_t len = response_len(p, reply_len - at), head;
        int ok;

        snprintf(label, sizeof(label), "%s 응답 %zu: %.*s", name, i + 1, (int)strlen(want[i].line) - 2, want[i].line);
        if (len == 0) {
            check(label, 0, "응답이 없거나 잘렸다");
            return;
        }
        head = find_in(p, len, "\r\n\r\n") + 4;
        ok = len >= strlen(want[i].line) && memcmp(p, want[i].line, strlen(want[i].line)) == 0;
        ok = ok && len - head == want[i].body_len && memcmp(p + head, want[i].body, want[i].body_len) == 0;
        if (want[i].conn == NULL) {
            ok = ok && find_in(p, head, "\r\nConnection:") == NPOS;
        } else {
            snprintf(conn, sizeof(conn), "\r\nConnection: %s\r\n", want[i].conn);
            ok = ok && find_in(p, head, conn) != NPOS;
        }
        ok = ok && (want[i].must == NULL || find_in(p, head, want[i].must) != NPOS);
        check(label, ok, "상태 줄·본문·Connection·머리가 다르다");
        at += len;
    }
    snprintf(label, sizeof(label), "%s 응답 %zu개 뒤 남은 바이트 없음", name, n);
    check(label, at == reply_len, "남은 바이트가 있다");
}

static double elapsed_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
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
        r = drive(fds[1], root);
        close(fds[1]);
        _exit(r == HTTP_CONN_DONE ? 0 : r == HTTP_CONN_PEER_CLOSED ? 1 : 2);
    }
    close(fds[1]);
    wrote = try_write_all(fds[0], "GET / HTTP/1.1\r\n", 16) == 0;
    nanosleep(&gap, NULL);
    wrote = wrote && try_write_all(fds[0], "Host: x\r\n\r\n", 11) == 0;
    setup_or_die(shutdown(fds[0], SHUT_WR) == 0, "shutdown");
    check("TC-2-07 두 조각 모두 쓰기 성공", wrote, "서버가 요청 도중 닫았다");
    reply_len = read_all(fds[0], reply, sizeof(reply));
    close(fds[0]);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    check("TC-2-07 나눠 쓴 요청에 200", starts_with(reply, "HTTP/1.1 200 OK\r\n"), "200 응답이 아니다");
    check("TC-2-07 나눠 쓴 요청 본문 = index.html", body_is(reply, reply_len, index_body, sizeof(index_body) - 1),
          "본문이 다르다");
    check("TC-2-07 나눠 쓴 요청 DONE", WIFEXITED(status) && WEXITSTATUS(status) == 0, "DONE이 아니다");
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
    r = drive(fds[1], root);
    close(fds[1]);
    n = read_all(fds[0], reply, sizeof(reply));
    close(fds[0]);
    check("TC-2-07 머리 도중 닫기 PEER_CLOSED", r == HTTP_CONN_PEER_CLOSED, "PEER_CLOSED가 아니다");
    check("TC-2-07 머리 도중 닫기 응답 없음", n == 0, "응답 바이트가 있다");
}

/*
 * D-17 보조: 결과별 상태 코드, 8192바이트로 버퍼가 꽉 찬 경우(0바이트 read 없이 400).
 * D-26 3단계: static_file 결과별 상태 코드. 본문과 Content-Length도 비교한다.
 */
static void test_status_mapping(void)
{
    static const struct {
        const char *name;
        const char *req;
        const char *line;
        const char *body;
        const char *conn; /* D-51: 닫는 코드는 close, 1.1 유지 응답은 Connection 머리 없음 */
        const char *must;
    } cases[] = {
        {"D-17 OK → 200 index.html", "GET / HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 200 OK\r\n", index_body, NULL,
         NULL},
        {"D-36 POST / 본문 있음 → 405 Allow: GET",
         "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc", "HTTP/1.1 405 Method Not Allowed\r\n",
         "405 Method Not Allowed\n", NULL, "\r\nAllow: GET\r\n"},
        {"D-36 POST /index.html 본문 있음 → 405 Allow: GET",
         "POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\nname=hyunji",
         "HTTP/1.1 405 Method Not Allowed\r\n", "405 Method Not Allowed\n", NULL, "\r\nAllow: GET\r\n"},
        {"D-26 REDIRECT → 301", "GET /sub HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 301 Moved Permanently\r\n",
         "301 Moved Permanently\n", NULL, NULL},
        {"D-26 BAD_TARGET → 400", "GET index.html HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 400 Bad Request\r\n",
         "400 Bad Request\n", "close", NULL},
        {"D-26 FORBIDDEN → 403", "GET /../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 403 Forbidden\r\n",
         "403 Forbidden\n", NULL, NULL},
        {"D-26 NOT_FOUND → 404", "GET /nope.html HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n",
         "404 Not Found\n", NULL, NULL},
        {"D-26 ERROR → 500 (ELOOP)", "GET /loop HTTP/1.1\r\nHost: x\r\n\r\n",
         "HTTP/1.1 500 Internal Server Error\r\n", "500 Internal Server Error\n", NULL, NULL},
        {"D-35 HEAD / HTTP/1.1 → 501 본문 그대로", "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n",
         "HTTP/1.1 501 Not Implemented\r\n", "501 Not Implemented\n", "close", NULL},
        {"D-17 BAD_REQUEST → 400", "GET / HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request\r\n", "400 Bad Request\n",
         "close", NULL},
        {"D-17 NOT_IMPLEMENTED → 501", "BREW / HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 501 Not Implemented\r\n",
         "501 Not Implemented\n", "close", NULL},
        {"D-17 VERSION_NOT_SUPPORTED → 505", "GET / HTTP/2.0\r\nHost: x\r\n\r\n",
         "HTTP/1.1 505 HTTP Version Not Supported\r\n", "505 HTTP Version Not Supported\n", "close", NULL},
        {"TC-4-21 chunked → 501, 닫힘", "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n",
         "HTTP/1.1 501 Not Implemented\r\n", "501 Not Implemented\n", "close", NULL},
        {"TC-4-21 chunked, gzip → 400, 닫힘",
         "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, gzip\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n",
         "HTTP/1.1 400 Bad Request\r\n", "400 Bad Request\n", "close", NULL},
        {"D-49 Content-Length 중복 → 400, 닫힘",
         "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
         "HTTP/1.1 400 Bad Request\r\n", "400 Bad Request\n", "close", NULL},
        {"D-44 1MB 초과 → 413, 닫힘", "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\nabc",
         "HTTP/1.1 413 Content Too Large\r\n", "413 Content Too Large\n", "close", NULL},
    };
    static char reply[4096], full[HTTP_REQUEST_HEAD_MAX];
    size_t i, reply_len;
    http_conn_result r;
    char label[160];

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        want_resp w = {cases[i].line, cases[i].body, strlen(cases[i].body), cases[i].conn, cases[i].must};
        r = serve_prewritten_len(cases[i].req, strlen(cases[i].req), reply, sizeof(reply), &reply_len);
        snprintf(label, sizeof(label), "%s DONE", cases[i].name);
        check(label, r == HTTP_CONN_DONE, "DONE이 아니다");
        check_replies(cases[i].name, reply, reply_len, &w, 1);
    }
    {
        const char req[] = "GET //sub?v=1 HTTP/1.1\r\nHost: x\r\n\r\n";
        r = serve_prewritten(req, sizeof(req) - 1, reply, sizeof(reply));
    }
    check("D-26 301 Location: /sub/ (C1, D-24), 1.1 유지라 Connection 없음",
          r == HTTP_CONN_DONE && strstr(reply, "\r\nLocation: /sub/\r\n\r\n") != NULL, reply);

    memset(full, 'a', sizeof(full));
    r = serve_prewritten(full, sizeof(full), reply, sizeof(reply));
    check("D-42 종료 없이 8192바이트 → 431 (0바이트 read 없음)",
          r == HTTP_CONN_DONE && starts_with(reply, "HTTP/1.1 431 Request Header Fields Too Large\r\n") &&
              strstr(reply, "\r\nConnection: close\r\n") != NULL,
          r == HTTP_CONN_PEER_CLOSED ? "PEER_CLOSED로 끝났다" : "431 응답이 아니다");

    memcpy(full, "GET /", 5);
    memset(full + 5, 'a', HTTP_REQUEST_TARGET_MAX);
    r = serve_prewritten(full, 4 + HTTP_REQUEST_TARGET_MAX + 1, reply, sizeof(reply));
    check("D-43 종료 빈 줄 없이 대상 2049바이트 → 414, 닫힘",
          r == HTTP_CONN_DONE && starts_with(reply, "HTTP/1.1 414 URI Too Long\r\n") &&
              strstr(reply, "\r\nConnection: close\r\n") != NULL,
          reply);
}

/* D-17·D-29: 파일 응답의 머리 쓰기 실패. 파일은 열렸으므로 fd 누수도 본다(TC-3-08). */
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
    r = drive(fds[1], root);
    err = errno;
    check("TC-3-08 머리 쓰기 실패 뒤 파일 fd 누수 없음", lowest_free_fd() == before, "열린 fd가 남았다");
    close(fds[1]);
    check("D-17 응답(파일 머리) 쓰기 실패 ERROR, errno EPIPE", r == HTTP_CONN_ERROR && err == EPIPE, strerror(err));
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
    setup_or_die(shutdown(fds[0], SHUT_WR) == 0, "shutdown");
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        http_conn_result r;
        close(fds[0]);
        r = drive(fds[1], root);
        close(fds[1]);
        _exit(r == HTTP_CONN_DONE ? 0 : 1);
    }
    close(fds[1]);
    reply_len = read_all(fds[0], reply, cap);
    close(fds[0]);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    check("TC-3-10 3×64KB+123 파일 DONE", WIFEXITED(status) && WEXITSTATUS(status) == 0, "자식 실패");
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
        r = drive(fds[1], root);
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

#define S(lit) lit, sizeof(lit) - 1 /* 문자열 상수와 그 길이 */
#define INDEX_200 {"HTTP/1.1 200 OK\r\n", index_body, sizeof(index_body) - 1, NULL, NULL}
#define GET_ROOT "GET / HTTP/1.1\r\nHost: x\r\n\r\n"

/* 미리 쓴 요청 하나의 결과가 DONE이고 응답들이 want와 같은가. */
static void expect_conn(const char *name, const char *req, size_t req_len, const want_resp *want, size_t n)
{
    static char reply[65536];
    char label[160];
    size_t reply_len;
    http_conn_result r = serve_prewritten_len(req, req_len, reply, sizeof(reply), &reply_len);

    snprintf(label, sizeof(label), "%s DONE", name);
    check(label, r == HTTP_CONN_DONE, "DONE이 아니다");
    check_replies(name, reply, reply_len, want, n);
}

/* TC-4-10: /echo 요청과 다음 요청이 한 번에 온다. */
static void test_two_requests_at_once(void)
{
    const want_resp want[] = {
        {"HTTP/1.1 200 OK\r\n", S("hello"), NULL, "\r\nContent-Type: application/octet-stream\r\n"},
        INDEX_200,
    };
    expect_conn("TC-4-10 POST /echo + GET / 한 번에",
                S("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello" GET_ROOT), want, 2);
}

/* TC-4-19: Expect: 100-continue. 1.1은 서버가 100을 보낸 뒤에야 본문을 보낸다(본문 읽기 직전에 보내는지 확인). */
static void test_expect_continue(void)
{
    static const char want100[] = "HTTP/1.1 100 Continue\r\n\r\n";
    static char reply[4096];
    char got[sizeof(want100) - 1];
    size_t have = 0, reply_len;
    int fds[2], status;
    pid_t pid;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        http_conn_result r;
        close(fds[0]);
        r = drive(fds[1], root);
        close(fds[1]);
        _exit(r == HTTP_CONN_DONE ? 0 : 1);
    }
    close(fds[1]);
    write_all(fds[0], S("POST /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-Continue\r\nContent-Length: 5\r\n\r\n"));
    while (have < sizeof(got)) {
        ssize_t n = read(fds[0], got + have, sizeof(got) - have);
        if (n <= 0) {
            break;
        }
        have += (size_t)n;
    }
    check("TC-4-19 1.1 본문 있음: 본문을 보내기 전에 100 Continue", have == sizeof(got) &&
                                                                     memcmp(got, want100, have) == 0,
          "100을 받지 못했다");
    /* 서버가 100 없이 먼저 닫았을 수 있으므로 쓰기 실패는 준비 실패로 보지 않는다(응답 비교에서 잡힌다). */
    (void)try_write_all(fds[0], S("hello"));
    (void)shutdown(fds[0], SHUT_WR);
    reply_len = read_all(fds[0], reply, sizeof(reply));
    close(fds[0]);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    {
        const want_resp want[] = {{"HTTP/1.1 200 OK\r\n", S("hello"), NULL, NULL}};
        check_replies("TC-4-19 1.1 100 뒤", reply, reply_len, want, 1);
    }
    check("TC-4-19 1.1 100 뒤 DONE", WIFEXITED(status) && WEXITSTATUS(status) == 0, "DONE이 아니다");

    {
        const want_resp want[] = {{"HTTP/1.1 200 OK\r\n", S("hello"), "close", NULL}};
        expect_conn("TC-4-19 1.0 본문 있음: 100 없음",
                    S("POST /echo HTTP/1.0\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\nhello"), want, 1);
    }
    {
        const want_resp want[] = {{"HTTP/1.1 200 OK\r\n", S(""), NULL, NULL}};
        expect_conn("TC-4-19 Content-Length: 0: 100 없음",
                    S("POST /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 0\r\n\r\n"), want, 1);
    }
    {
        const want_resp want[] = {{"HTTP/1.1 413 Content Too Large\r\n", S("413 Content Too Large\n"), "close", NULL}};
        expect_conn("TC-4-19 1MB 초과: 100 없이 413",
                    S("POST /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 1048577\r\n\r\n"), want,
                    1);
    }
}

/* TC-4-08 보조: Connection·버전별 유지(D-51). 닫은 뒤의 요청에는 응답하지 않는다. */
static void test_keep_alive_rules(void)
{
    {
        const want_resp want[] = {{"HTTP/1.1 200 OK\r\n", index_body, sizeof(index_body) - 1, "close", NULL}};
        expect_conn("D-51 1.1 Connection: close → 응답 하나 뒤 닫힘",
                    S("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" GET_ROOT), want, 1);
    }
    {
        const want_resp want[] = {
            {"HTTP/1.1 200 OK\r\n", index_body, sizeof(index_body) - 1, "keep-alive", NULL},
            {"HTTP/1.1 200 OK\r\n", index_body, sizeof(index_body) - 1, "close", NULL},
        };
        expect_conn("D-51 1.0 keep-alive 뒤 1.0 → 유지 후 닫힘",
                    S("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET / HTTP/1.0\r\n\r\n" GET_ROOT), want, 2);
    }
}

/* TC-4-22: Content-Length 밖에 덧붙은 빈 줄은 다음 요청의 앞 빈 줄이라 400(D-48). */
static void test_trailing_crlf(void)
{
    const want_resp want[] = {
        {"HTTP/1.1 200 OK\r\n", S("hello"), NULL, NULL},
        {"HTTP/1.1 400 Bad Request\r\n", S("400 Bad Request\n"), "close", NULL},
    };
    expect_conn("TC-4-22 본문 뒤 \\r\\n + GET /",
                S("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello\r\n" GET_ROOT), want, 2);
}

/* TC-4-24: 머리 + 8KB를 넘는 /echo 본문 + GET /을 한 번에 쓴다(D-47 본문 일부만 버퍼에 있는 경우). */
static void test_big_echo_then_next(void)
{
    const size_t body_len = 3 * HTTP_REQUEST_HEAD_MAX + 100;
    const size_t cap = 2 * body_len + 4096;
    char *req = malloc(cap), *reply = malloc(cap), head[128];
    size_t i, len, reply_len;
    int fds[2], status;
    pid_t pid;

    setup_or_die(req != NULL && reply != NULL, "malloc");
    len = (size_t)snprintf(head, sizeof(head), "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", body_len);
    memcpy(req, head, len);
    for (i = 0; i < body_len; i++) {
        req[len + i] = (char)(i * 131 + 17);
    }
    memcpy(req + len + body_len, GET_ROOT, strlen(GET_ROOT));
    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        http_conn_result r;
        close(fds[0]);
        r = drive(fds[1], root);
        close(fds[1]);
        _exit(r == HTTP_CONN_DONE ? 0 : 1);
    }
    close(fds[1]);
    write_all(fds[0], req, len + body_len + strlen(GET_ROOT));
    setup_or_die(shutdown(fds[0], SHUT_WR) == 0, "shutdown");
    reply_len = read_all(fds[0], reply, cap);
    close(fds[0]);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    check("TC-4-24 큰 /echo + GET / DONE", WIFEXITED(status) && WEXITSTATUS(status) == 0, "DONE이 아니다");
    {
        const want_resp want[] = {{"HTTP/1.1 200 OK\r\n", req + len, body_len, NULL, NULL}, INDEX_200};
        check_replies("TC-4-24 큰 /echo + GET /", reply, reply_len, want, 2);
    }
    free(req);
    free(reply);
}

/* TC-4-25: /echo 판정 경계(D-50). TC-4-05 보조: GET /echo → 405 Allow: POST. */
static void test_echo_boundaries(void)
{
    {
        const want_resp want[] = {{"HTTP/1.1 200 OK\r\n", S("abc"), NULL, NULL}};
        expect_conn("TC-4-25 POST /echo?x=1 → 200",
                    S("POST /echo?x=1 HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc"), want, 1);
        expect_conn("TC-4-25 루트에 echo 파일이 있어도 POST /echo → 되돌림",
                    S("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc"), want, 1);
    }
    {
        const want_resp want[] = {
            {"HTTP/1.1 405 Method Not Allowed\r\n", S("405 Method Not Allowed\n"), NULL, "\r\nAllow: GET\r\n"}, INDEX_200};
        expect_conn("TC-4-25 POST /echo/ → 405 Allow: GET, 유지",
                    S("POST /echo/ HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc" GET_ROOT), want, 2);
        expect_conn("TC-4-25 없는 파일에 POST → 405, 유지",
                    S("POST /nope.html HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc" GET_ROOT), want, 2);
    }
    {
        const want_resp want[] = {
            {"HTTP/1.1 405 Method Not Allowed\r\n", S("405 Method Not Allowed\n"), NULL, "\r\nAllow: POST\r\n"},
            INDEX_200};
        expect_conn("TC-4-05 GET /echo → 405 Allow: POST, 유지", S("GET /echo HTTP/1.1\r\nHost: x\r\n\r\n" GET_ROOT),
                    want, 2);
        expect_conn("TC-4-25 GET /echo?x 본문 있음 → 405, 본문 버리고 유지",
                    S("GET /echo?x HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nzz" GET_ROOT), want, 2);
    }
}

/* TC-4-26: 유지·닫기 경계(D-51). */
static void test_keep_or_close_after_error(void)
{
    {
        const want_resp want[] = {
            {"HTTP/1.1 500 Internal Server Error\r\n", S("500 Internal Server Error\n"), NULL, NULL}, INDEX_200};
        expect_conn("TC-4-26 500 뒤 유지", S("GET /loop HTTP/1.1\r\nHost: x\r\n\r\n" GET_ROOT), want, 2);
    }
    {
        const want_resp want[] = {
            {"HTTP/1.1 301 Moved Permanently\r\n", S("301 Moved Permanently\n"), NULL, "\r\nLocation: /sub/\r\n"},
            INDEX_200};
        expect_conn("TC-4-26 301 뒤 유지", S("GET /sub HTTP/1.1\r\nHost: x\r\n\r\n" GET_ROOT), want, 2);
    }
    {
        const want_resp want[] = {{"HTTP/1.1 403 Forbidden\r\n", S("403 Forbidden\n"), NULL, NULL}, INDEX_200};
        expect_conn("TC-4-26 403 뒤 유지", S("GET /../x HTTP/1.1\r\nHost: x\r\n\r\n" GET_ROOT), want, 2);
    }
    {
        const want_resp want[] = {{"HTTP/1.1 404 Not Found\r\n", S("404 Not Found\n"), NULL, NULL}, INDEX_200};
        expect_conn("TC-4-09 보조: 404 뒤 유지", S("GET /nope HTTP/1.1\r\nHost: x\r\n\r\n" GET_ROOT), want, 2);
    }
    {
        const want_resp want[] = {{"HTTP/1.1 400 Bad Request\r\n", S("400 Bad Request\n"), "close", NULL}};
        expect_conn("TC-4-26 정적 판정 400 뒤 닫힘", S("GET index.html HTTP/1.1\r\nHost: x\r\n\r\n" GET_ROOT), want, 1);
    }
}

/* TC-4-27: 본문 있는 GET도 본문을 읽고 버린 뒤 다음 요청을 처리한다(D-49). */
static void test_get_with_body(void)
{
    const want_resp want[] = {INDEX_200, INDEX_200};
    expect_conn("TC-4-27 본문 있는 GET 뒤 GET", S("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc" GET_ROOT),
                want, 2);
}

/* TC-4-28: 버릴 본문 도중 상대가 닫으면 응답 없이 PEER_CLOSED(D-50). */
static void test_close_mid_discard(void)
{
    static char reply[4096];
    size_t reply_len;
    http_conn_result r =
        serve_prewritten_len(S("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc"), reply, sizeof(reply), &reply_len);
    check("TC-4-28 버릴 본문 도중 닫힘 PEER_CLOSED", r == HTTP_CONN_PEER_CLOSED, "PEER_CLOSED가 아니다");
    check("TC-4-28 응답 없음", reply_len == 0, reply);
}

/* ---- 서버를 자식으로 띄우고 결과·errno를 파이프로 받는 시험(TC-4-32·34·35) ---- */

typedef struct {
    http_conn_result result;
    int err;
} served;

/* fds[0]은 클라이언트 쪽이다. 테스트가 서버를 기다리다 멈추지 않게 읽기 시간 제한 3초를 건다. */
static pid_t fork_server(int fds[2], int *result_fd)
{
    struct timeval tv = {3, 0};
    int p[2];
    pid_t pid;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    setup_or_die(pipe(p) == 0, "pipe");
    setup_or_die(setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0, "SO_RCVTIMEO");
    fflush(stdout);
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        served s;
        close(fds[0]);
        close(p[0]);
        errno = 0;
        s.result = drive(fds[1], root);
        s.err = errno;
        close(fds[1]);
        _exit(write(p[1], &s, sizeof(s)) == (ssize_t)sizeof(s) ? 0 : 3);
    }
    close(fds[1]);
    close(p[1]);
    *result_fd = p[0];
    return pid;
}

static served join_server(pid_t pid, int result_fd)
{
    served s = {HTTP_CONN_ERROR, -1}; /* 자식이 결과를 못 보내면 err -1 */
    int status;

    if (read(result_fd, &s, sizeof(s)) != (ssize_t)sizeof(s)) {
        s.result = HTTP_CONN_ERROR;
        s.err = -1;
    }
    close(result_fd);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    return s;
}

/* 완성된 응답이 count개가 될 때까지 읽는다. EOF·시간 제한이면 그때까지. 전체 길이를 돌려준다. */
static size_t read_responses(int fd, char *buf, size_t cap, size_t have, size_t count)
{
    for (;;) {
        size_t at = 0, k = 0, l;
        ssize_t n;
        while (k < count && (l = response_len(buf + at, have - at)) > 0) {
            at += l;
            k++;
        }
        if (k == count || have == cap) {
            return have;
        }
        n = read(fd, buf + have, cap - have);
        if (n <= 0) {
            return have;
        }
        have += (size_t)n;
    }
}

/* needle이 보일 때까지 읽는다. EOF·시간 제한이면 그때까지. */
static size_t read_until_found(int fd, char *buf, size_t cap, size_t have, const char *needle)
{
    while (find_in(buf, have, needle) == NPOS && have < cap) {
        ssize_t n = read(fd, buf + have, cap - have);
        if (n <= 0) {
            break;
        }
        have += (size_t)n;
    }
    return have;
}

/*
 * TC-4-34 (N2, P1): 첫 응답 뒤 남은 바이트가 다음 요청 머리 일부다. 첫 응답을 받은 뒤에야 나머지를 쓰므로
 * 서버는 남은 바이트를 맨 앞으로 옮기고 다시 read해야 둘째 요청을 완성한다(D-47).
 */
static void test_leftover_partial_head(void)
{
    static char reply[16384];
    const want_resp want[] = {INDEX_200, INDEX_200};
    size_t have;
    int fds[2], rfd;
    pid_t pid = fork_server(fds, &rfd);
    served s;

    write_all(fds[0], S(GET_ROOT "GET / HT"));
    have = read_responses(fds[0], reply, sizeof(reply), 0, 1);
    check("TC-4-34 남은 바이트(머리 일부)만 있을 때 첫 응답이 먼저 옴", response_len(reply, have) == have && have > 0,
          "첫 응답이 완성되지 않았거나 더 왔다");
    write_all(fds[0], S("TP/1.1\r\nHost: x\r\n\r\n"));
    setup_or_die(shutdown(fds[0], SHUT_WR) == 0, "shutdown");
    have = read_responses(fds[0], reply, sizeof(reply), have, 3);
    close(fds[0]);
    s = join_server(pid, rfd);
    check("TC-4-34 이동 뒤 다시 읽어 둘째 요청 완성, DONE", s.result == HTTP_CONN_DONE, "DONE이 아니다");
    check_replies("TC-4-34 남은 머리 일부 + 나중 바이트", reply, have, want, 2);
}

/*
 * TC-4-35 (N2, P2): 남은 바이트가 다음 /echo 머리 + 본문 일부다. 서버가 그 일부(abc)를 돌려준 것을 본 뒤에
 * 나머지 본문과 다음 요청을 쓴다. 본문 나머지는 소켓에서 남은 길이만큼만 읽어야 셋째 요청이 남는다(D-47).
 */
static void test_leftover_head_and_partial_body(void)
{
    static char reply[16384];
    const want_resp want[] = {INDEX_200, {"HTTP/1.1 200 OK\r\n", S("abcdefghij"), NULL, NULL}, INDEX_200};
    size_t have;
    int fds[2], rfd;
    pid_t pid = fork_server(fds, &rfd);
    served s;

    write_all(fds[0], S(GET_ROOT "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc"));
    have = read_responses(fds[0], reply, sizeof(reply), 0, 1);
    have = read_until_found(fds[0], reply, sizeof(reply), have, "\r\n\r\nabc");
    check("TC-4-35 버퍼에 있던 본문 일부(abc)가 나머지보다 먼저 나감", find_in(reply, have, "\r\n\r\nabc") != NPOS &&
                                                                       have >= 3 && memcmp(reply + have - 3, "abc", 3) == 0,
          "abc가 먼저 오지 않았다");
    write_all(fds[0], S("defghij" GET_ROOT));
    setup_or_die(shutdown(fds[0], SHUT_WR) == 0, "shutdown");
    have = read_responses(fds[0], reply, sizeof(reply), have, 4);
    close(fds[0]);
    s = join_server(pid, rfd);
    check("TC-4-35 본문 나머지 읽은 뒤 다음 요청, DONE", s.result == HTTP_CONN_DONE, "DONE이 아니다");
    check_replies("TC-4-35 머리 + 본문 일부 + 나중 나머지", reply, have, want, 3);
}

/* TC-4-36 (N2, P9): 머리가 정확히 8192바이트면 버퍼가 꽉 차고 본문은 모두 소켓에 있다. 8193이면 431. */
static void test_head_exactly_8k(void)
{
    static char req[HTTP_REQUEST_HEAD_MAX + 64];
    const char pre[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nX-Pad: ";
    size_t pad = HTTP_REQUEST_HEAD_MAX - (sizeof(pre) - 1) - 4, len;

    memcpy(req, pre, sizeof(pre) - 1);
    memset(req + sizeof(pre) - 1, 'p', pad);
    memcpy(req + sizeof(pre) - 1 + pad, "\r\n\r\n", 4);
    check("TC-4-36 준비: 머리 8192바이트", find_in(req, HTTP_REQUEST_HEAD_MAX, "\r\n\r\n") + 4 == HTTP_REQUEST_HEAD_MAX,
          "길이가 다르다");
    len = HTTP_REQUEST_HEAD_MAX;
    memcpy(req + len, "hello" GET_ROOT, 5 + strlen(GET_ROOT));
    {
        const want_resp want[] = {{"HTTP/1.1 200 OK\r\n", S("hello"), NULL, NULL}, INDEX_200};
        expect_conn("TC-4-36 머리 8192 + 본문 5 + 다음 요청", req, len + 5 + strlen(GET_ROOT), want, 2);
    }
    req[HTTP_REQUEST_HEAD_MAX - 4] = 'p';
    memcpy(req + HTTP_REQUEST_HEAD_MAX - 3, "\r\n\r\nhello", 9);
    {
        const want_resp want[] = {{"HTTP/1.1 431 Request Header Fields Too Large\r\n",
                                   S("431 Request Header Fields Too Large\n"), "close", NULL}};
        expect_conn("TC-4-36 머리 8193 → 431, 닫힘", req, HTTP_REQUEST_HEAD_MAX + 6, want, 1);
    }
}

/* ---- Step 5: 연결 상태 구조체를 직접 부르는 시험(가짜 시계) ---- */

#define SEC 1000000000LL

/* 서버 쪽 소켓 하나와 연결 상태. 서버 쪽은 논블로킹, 클라이언트 쪽은 블로킹 그대로다. */
typedef struct {
    int client, server;
    http_conn *c;
} direct;

static direct direct_open(long long now)
{
    direct d;
    int fds[2];

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    d.client = fds[0];
    d.server = fds[1];
    set_nonblocking(d.server);
    d.c = malloc(sizeof(*d.c));
    setup_or_die(d.c != NULL, "malloc");
    http_conn_init(d.c, d.server, root, NULL, now);
    return d;
}

static void direct_close(direct *d)
{
    http_conn_release(d->c);
    free(d->c);
    close(d->server);
    if (d->client >= 0) {
        close(d->client);
    }
}

/* 기다림 없이 진행할 수 있는 동안(RUNNABLE, 쓸 수 있는 WANT_WRITE) 차례를 반복한다. 최대 max번. */
static http_conn_step run_ready(http_conn *c, long long now, int max)
{
    http_conn_step step = HTTP_CONN_RUNNABLE;
    int i;

    for (i = 0; i < max; i++) {
        struct pollfd pfd = {c->fd, POLLOUT, 0};
        step = http_conn_run(c, HTTP_CONN_WAKE_CLIENT, now);
        if (step == HTTP_CONN_RUNNABLE) {
            continue;
        }
        if (step != HTTP_CONN_WANT_WRITE || poll(&pfd, 1, 0) != 1) {
            break;
        }
    }
    return step;
}

/* 클라이언트 쪽에 지금 와 있는 바이트를 모두 읽는다(기다리지 않음). */
static size_t read_available(int fd, char *buf, size_t cap)
{
    size_t have = 0;
    for (;;) {
        ssize_t n = recv(fd, buf + have, cap - have, MSG_DONTWAIT);
        if (n <= 0) {
            return have;
        }
        have += (size_t)n;
        setup_or_die(have < cap, "응답이 테스트 버퍼보다 크다");
    }
}

/*
 * TC-5-09: EOF·추가 송신 없이 한 번에 쓴 요청. 버퍼에 남은 일을 다 처리하기 전에 읽기 대기(WANT_READ)로
 * 돌아가면 아직 안 나간 응답이 생기므로, 첫 WANT_READ 시점에 클라이언트가 받은 바이트로 판정한다.
 */
static void buffered_case(const char *name, const char *req, size_t len, const want_resp *want, size_t n)
{
    static char reply[65536];
    char label[160];
    direct d = direct_open(0);
    http_conn_step step;
    size_t have;

    write_all(d.client, req, len);
    step = run_ready(d.c, 0, 1000);
    have = read_available(d.client, reply, sizeof(reply));
    snprintf(label, sizeof(label), "%s: 연결 유지, 읽기 대기로 멈춤", name);
    check(label, step == HTTP_CONN_WANT_READ, step == HTTP_CONN_FINISHED ? "끝났다" : "읽기 대기가 아니다");
    check_replies(name, reply, have, want, n);
    direct_close(&d);
}

static void test_buffered_work(void)
{
    {
        const want_resp want[] = {INDEX_200, {"HTTP/1.1 404 Not Found\r\n", S("404 Not Found\n"), NULL, NULL}};
        buffered_case("TC-5-09 완성 요청 2개 한 번에", S(GET_ROOT "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n"), want, 2);
    }
    {
        const want_resp want[] = {{"HTTP/1.1 100 Continue\r\n", S(""), NULL, NULL}, {"HTTP/1.1 200 OK\r\n", S("hello"), NULL, NULL}};
        buffered_case("TC-5-09 Expect 요청 + 본문 한 번에",
                      S("POST /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\nhello"), want, 2);
    }
    {
        static char reply[4096];
        direct d = direct_open(0);
        http_conn_step step;
        size_t have;

        write_all(d.client, S("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc"));
        step = run_ready(d.c, 0, 1000);
        have = read_available(d.client, reply, sizeof(reply));
        check("TC-5-09 /echo 머리 + 본문 일부: 읽기 대기로 멈춤", step == HTTP_CONN_WANT_READ, "읽기 대기가 아니다");
        check("TC-5-09 /echo 머리 + 본문 일부: 머리(Content-Length: 10)와 abc가 먼저 나감",
              starts_with(reply, "HTTP/1.1 200 OK\r\n") && find_in(reply, have, "\r\nContent-Length: 10\r\n") != NPOS &&
                  find_in(reply, have, "\r\n\r\nabc") + 7 == have,
              "머리·abc가 다 오지 않았다");
        direct_close(&d);
    }
}

/* 연결이 기다리는 중일 때 마감을 확인하고, 지났으면 만료 처리한 뒤 1을 돌려준다(루프의 마감 확인과 같다). */
static int expire_if_due(http_conn *c, long long now)
{
    if (!http_conn_expired(c, now)) {
        return 0;
    }
    http_conn_expire(c);
    return 1;
}

/* TC-5-10: 첫 write부터 막힘 → 채운 때부터 5초 뒤 ERROR(EAGAIN). 서버→클라이언트 방향을 미리 가득 채운다. */
static void deadline_first_write_blocked(void)
{
    static char junk[4096];
    direct d = direct_open(0);
    http_conn_step step;

    memset(junk, 'j', sizeof(junk));
    while (write(d.server, junk, sizeof(junk)) > 0) {
    }
    setup_or_die(errno == EAGAIN || errno == EWOULDBLOCK, "서버 쪽 송신 공간 채우기");
    write_all(d.client, S(GET_ROOT));
    step = run_ready(d.c, 1 * SEC, 10);
    check("TC-5-10 첫 write부터 막힘: 쓰기 대기, 한 바이트도 못 보냄", step == HTTP_CONN_WANT_WRITE && d.c->out_pos == 0 &&
                                                                    d.c->out_len > 0,
          "쓰기 대기가 아니다");
    check("TC-5-10 첫 write부터 막힘: 채운 때+5초 직전에는 만료 아님", !expire_if_due(d.c, 6 * SEC - 1), "일찍 만료됐다");
    check("TC-5-10 첫 write부터 막힘: 채운 때+5초에 ERROR(EAGAIN)",
          expire_if_due(d.c, 6 * SEC) && d.c->result == HTTP_CONN_ERROR && d.c->err == EAGAIN, "결과가 다르다");
    direct_close(&d);
}

/* TC-5-10: 100을 보낸 때부터 읽기 마감, 그 뒤 4초마다 1바이트씩 총 40초 넘게 본문 → 최종 응답 405. */
static void deadline_slow_body_after_continue(void)
{
    static char reply[4096];
    direct d = direct_open(0);
    http_conn_step step;
    size_t have;
    int i, early = 0;

    write_all(d.client, S("POST /x HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 10\r\n\r\n"));
    step = http_conn_run(d.c, HTTP_CONN_WAKE_CLIENT, 0); /* 머리를 읽고 100을 채운다(RUNNABLE) */
    step = http_conn_run(d.c, HTTP_CONN_WAKE_CLIENT, 1 * SEC); /* 100을 다 보낸 때 = 1초 */
    have = read_available(d.client, reply, sizeof(reply));
    check("TC-5-10 100 뒤: 100만 나가고 본문 읽기 대기", step == HTTP_CONN_WANT_READ && have == 25 &&
                                                       memcmp(reply, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0,
          "100·대기가 다르다");
    check("TC-5-10 100 뒤: 읽기 마감은 100을 보낸 때부터(머리 받은 0초+5초에는 아직 아님)", !expire_if_due(d.c, 5 * SEC + SEC / 2),
          "머리 받은 때부터 잰다");
    for (i = 0; i < 10; i++) {
        long long t = 5 * SEC + SEC / 2 + (long long)i * 4 * SEC; /* 5.5초부터 4초 간격 */
        early |= expire_if_due(d.c, t);
        write_all(d.client, "x", 1);
        step = run_ready(d.c, t, 10);
    }
    have = read_available(d.client, reply, sizeof(reply));
    check("TC-5-10 조금씩 41초 넘게 받아도 진행마다 마감 갱신(만료 없음)", !early, "도중에 만료됐다");
    check("TC-5-10 본문을 다 받은 뒤 최종 응답 405, 연결 유지",
          step == HTTP_CONN_WANT_READ && starts_with(reply, "HTTP/1.1 405 Method Not Allowed\r\n") &&
              response_len(reply, have) == have,
          "405가 아니다");
    direct_close(&d);
}

/* TC-5-10: /echo 출력 막힘(클라이언트가 받아 가지 않음) → 마지막 쓰기 진행+5초에 ERROR(EAGAIN). */
static void deadline_echo_output_blocked(void)
{
    const size_t body_len = HTTP_CONTENT_LENGTH_MAX;
    char head[128], *body = malloc(body_len);
    direct d = direct_open(0);
    http_conn_step step = HTTP_CONN_WANT_READ;
    struct pollfd pfd;
    size_t sent = 0;
    int i, hl;

    setup_or_die(body != NULL, "malloc");
    memset(body, 'e', body_len);
    hl = snprintf(head, sizeof(head), "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", body_len);
    write_all(d.client, head, (size_t)hl);
    set_nonblocking(d.client);
    for (i = 0; i < 10000; i++) {
        ssize_t n = sent < body_len ? write(d.client, body + sent, body_len - sent) : 0;
        if (n > 0) {
            sent += (size_t)n;
        }
        step = run_ready(d.c, 2 * SEC, 10);
        pfd.fd = d.server;
        pfd.events = POLLOUT;
        if (step == HTTP_CONN_WANT_WRITE && poll(&pfd, 1, 0) == 0) {
            break;
        }
    }
    check("TC-5-10 /echo 출력 막힘: 본문 도중 쓰기 대기", step == HTTP_CONN_WANT_WRITE && d.c->body_left > 0,
          "쓰기 대기가 아니다");
    check("TC-5-10 /echo 출력 막힘: 마지막 진행+5초 직전에는 만료 아님", !expire_if_due(d.c, 7 * SEC - 1), "일찍 만료됐다");
    check("TC-5-10 /echo 출력 막힘: 마지막 진행+5초에 ERROR(EAGAIN)",
          expire_if_due(d.c, 7 * SEC) && d.c->result == HTTP_CONN_ERROR && d.c->err == EAGAIN, "결과가 다르다");
    direct_close(&d);
    free(body);
}

/* TC-5-10: /echo 입력 끊김 → 출력을 다 보낸 때+5초에 TIMED_OUT. 진행 없는 준비 알림(3초)은 마감을 늘리지 않는다. */
static void deadline_echo_input_stalled(void)
{
    static char reply[4096];
    direct d = direct_open(0);
    http_conn_step step;
    size_t have;

    write_all(d.client, S("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc"));
    step = run_ready(d.c, 0, 10);
    have = read_available(d.client, reply, sizeof(reply));
    check("TC-5-10 /echo 입력 끊김: 머리+abc 보낸 뒤 읽기 대기", step == HTTP_CONN_WANT_READ &&
                                                               find_in(reply, have, "\r\n\r\nabc") + 7 == have,
          "다르다");
    step = http_conn_run(d.c, HTTP_CONN_WAKE_CLIENT, 3 * SEC); /* 진행 없는 차례(EAGAIN) */
    check("TC-5-10 /echo 입력 끊김: 진행 없는 차례 뒤에도 기존 마감(5초) 유지",
          step == HTTP_CONN_WANT_READ && !expire_if_due(d.c, 5 * SEC - 1) && expire_if_due(d.c, 5 * SEC) &&
              d.c->result == HTTP_CONN_TIMED_OUT,
          "마감·결과가 다르다");
    direct_close(&d);
}

/* TC-5-10: 부분 쓰기(작은 송신 버퍼) 뒤에도 위치를 기억해 이어 보내 파일 바이트가 같다. */
static void deadline_partial_write_bytes(void)
{
    const size_t cap = BIG_LEN + 4096;
    char *reply = malloc(cap);
    direct d = direct_open(0);
    int small = 4096, partial = 0, i;
    size_t have = 0;
    http_conn_step step = HTTP_CONN_WANT_READ;

    setup_or_die(reply != NULL, "malloc");
    setup_or_die(setsockopt(d.server, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0, "SO_SNDBUF");
    write_all(d.client, S("GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n"));
    for (i = 0; i < 100000 && (have == 0 || response_len(reply, have) == 0); i++) {
        ssize_t n;
        step = http_conn_run(d.c, HTTP_CONN_WAKE_CLIENT, 0);
        if (step == HTTP_CONN_WANT_WRITE && d.c->out_pos > 0 && d.c->out_pos < d.c->out_len) {
            partial = 1;
        }
        n = recv(d.client, reply + have, 1000 < cap - have ? 1000 : cap - have, MSG_DONTWAIT);
        if (n > 0) {
            have += (size_t)n;
        }
    }
    check("TC-5-10 부분 쓰기 발생(보낸 위치가 출력 중간)", partial, "부분 쓰기를 보지 못했다");
    check("TC-5-10 부분 쓰기 뒤 파일 바이트·Content-Length 일치", body_is(reply, have, big_body, BIG_LEN), "본문이 다르다");
    check("TC-5-10 부분 쓰기 뒤 연결 유지(다음 요청 대기)", step == HTTP_CONN_WANT_READ, "다르다");
    direct_close(&d);
    free(reply);
}

/* TC-5-10: 5초 경계에 이번 배치의 준비 연결 — 실제 진행이 있으면 계속, 없으면 만료. 정리는 마감을 먼저 본다. */
static void deadline_ready_at_boundary(void)
{
    static char reply[4096];
    direct d;
    http_conn_step step;
    size_t have;
    int avail = -1;

    d = direct_open(0);
    write_all(d.client, S("GET / HT"));
    step = run_ready(d.c, 0, 10);
    write_all(d.client, S("TP/1.1\r\nHost: x\r\n\r\n"));
    check("TC-5-10 경계 진행: 마감(5초)이 지난 6초에 준비됨", step == HTTP_CONN_WANT_READ && http_conn_expired(d.c, 6 * SEC),
          "준비 조건이 다르다");
    step = run_ready(d.c, 6 * SEC, 10); /* 배치 처리가 마감 확인보다 먼저다 */
    have = read_available(d.client, reply, sizeof(reply));
    check("TC-5-10 경계 진행: 실제 진행이 있으면 이어 처리(응답, 만료 아님)",
          step == HTTP_CONN_WANT_READ && !expire_if_due(d.c, 6 * SEC) && starts_with(reply, "HTTP/1.1 200 OK\r\n") &&
              response_len(reply, have) == have,
          "만료됐거나 응답이 없다");
    direct_close(&d);

    d = direct_open(0);
    write_all(d.client, S("GET / HT"));
    (void)run_ready(d.c, 0, 10);
    step = http_conn_run(d.c, HTTP_CONN_WAKE_CLIENT, 6 * SEC); /* 준비 알림만 있고 바이트 없음 */
    check("TC-5-10 경계 미진행: 진행 없으면 마감 확인에서 TIMED_OUT",
          step == HTTP_CONN_WANT_READ && expire_if_due(d.c, 6 * SEC) && d.c->result == HTTP_CONN_TIMED_OUT, "다르다");
    direct_close(&d);

    d = direct_open(0);
    write_all(d.client, S("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n"));
    step = run_ready(d.c, 0, 10); /* 413 → shutdown → 정리(마감 5초 고정) */
    have = read_available(d.client, reply, sizeof(reply));
    write_all(d.client, "1", 1);
    step = run_ready(d.c, 4 * SEC, 10);
    check("TC-5-10 정리: 413 뒤 정리 중 4초에 받은 바이트는 읽어 버리고 계속",
          starts_with(reply, "HTTP/1.1 413 ") && step == HTTP_CONN_WANT_READ, "다르다");
    check("TC-5-10 정리: 읽어도 마감은 고정(5초)", http_conn_expired(d.c, 5 * SEC), "정리 마감이 늘었다");
    write_all(d.client, "2", 1);
    step = http_conn_run(d.c, HTTP_CONN_WAKE_CLIENT, 6 * SEC);
    setup_or_die(ioctl(d.server, FIONREAD, &avail) == 0, "FIONREAD");
    check("TC-5-10 정리: 마감 뒤 준비돼도 새로 읽지 않고 DONE",
          step == HTTP_CONN_FINISHED && d.c->result == HTTP_CONN_DONE && avail == 1, "마감 뒤에 읽었다");
    direct_close(&d);
}

static void test_deadlines(void)
{
    deadline_first_write_blocked();
    deadline_slow_body_after_continue();
    deadline_echo_output_blocked();
    deadline_echo_input_stalled();
    deadline_partial_write_bytes();
    deadline_ready_at_boundary();
}

/* TC-4-31 (N1): /echo 머리를 보낸 뒤 본문 도중 상대가 닫으면 PEER_CLOSED, 머리와 받은 본문만 나간다. */
static void test_echo_peer_closed_mid_body(void)
{
    static char reply[4096];
    size_t reply_len;
    http_conn_result r = serve_prewritten_len(S("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc"), reply,
                                              sizeof(reply), &reply_len);
    check("TC-4-31 /echo 본문 도중 닫힘 PEER_CLOSED", r == HTTP_CONN_PEER_CLOSED, "PEER_CLOSED가 아니다");
    check("TC-4-31 머리(Content-Length: 10) + abc만 나감",
          starts_with(reply, "HTTP/1.1 200 OK\r\n") && find_in(reply, reply_len, "\r\nContent-Length: 10\r\n") != NPOS &&
              find_in(reply, reply_len, "\r\n\r\nabc") + 7 == reply_len,
          reply);
}

/*
 * TC-4-32 (N1, P5·P6): ECONNRESET 분류. 응답 전체가 도착한 것을 MSG_PEEK로 확인한 뒤 읽지 않은 채 닫으면
 * AF_UNIX에서 서버의 다음 read가 ECONNRESET이 된다. 요청 사이면 DONE, 요청 도중(다음 머리 일부가 버퍼에 있음)이면 ERROR.
 */
static void econnreset_case(const char *name, const char *req, size_t len, http_conn_result want)
{
    char peek[4096], label[160];
    int fds[2], rfd;
    pid_t pid = fork_server(fds, &rfd);
    served s;

    write_all(fds[0], req, len);
    /* Step 5: 응답 전체가 도착한 것을 MSG_PEEK로만 확인한다(소비하지 않음). 그 뒤 서버의 다음 read가 ECONNRESET을 본다. */
    for (;;) {
        ssize_t n = recv(fds[0], peek, sizeof(peek), MSG_PEEK);
        setup_or_die(n > 0, "응답 도착 확인");
        if (response_len(peek, (size_t)n) > 0) {
            break;
        }
    }
    close(fds[0]); /* 읽지 않은 응답을 둔 채 닫는다 */
    s = join_server(pid, rfd);
    snprintf(label, sizeof(label), "%s, errno ECONNRESET", name);
    check(label, s.result == want && s.err == ECONNRESET, s.err > 0 ? strerror(s.err) : "결과가 다르다");
}

static void test_econnreset(void)
{
    econnreset_case("TC-4-32 요청 사이 ECONNRESET → DONE", S(GET_ROOT), HTTP_CONN_DONE);
    econnreset_case("TC-4-32 요청 도중 ECONNRESET → ERROR", S(GET_ROOT "GET /"), HTTP_CONN_ERROR);
}

/*
 * TC-4-33 (N1, P7): 100 Continue 쓰기 실패 → ERROR(EPIPE). /echo가 아닌 주소를 써서, 100 실패를 무시하면
 * 본문 버리기에서 EOF(PEER_CLOSED)가 되므로 100 쓰기 실패 경로와 구별된다.
 */
static void test_continue_write_failure(void)
{
    int fds[2], err;
    http_conn_result r;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], S("POST /x HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n"));
    close(fds[0]);
    errno = 0;
    r = drive(fds[1], root);
    err = errno;
    close(fds[1]);
    check("TC-4-33 100 쓰기 실패 ERROR, errno EPIPE", r == HTTP_CONN_ERROR && err == EPIPE, strerror(err));
}

/* ---- 5초를 기다리는 시험: 자식 프로세스에서 함께 돌린다 ---- */

/* 요청을 쓰고 쓰기 방향은 열어 둔 채 서버를 부른다. 결과·errno·걸린 시간·응답 길이를 돌려준다. */
static http_conn_result serve_open(const char *req, size_t len, int *err, double *secs, size_t *reply_len)
{
    static char reply[65536];
    struct timespec start;
    int fds[2];
    http_conn_result r;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], req, len);
    clock_gettime(CLOCK_MONOTONIC, &start);
    errno = 0;
    r = drive(fds[1], root);
    *err = errno;
    *secs = elapsed_since(&start);
    close(fds[1]);
    *reply_len = read_all(fds[0], reply, sizeof(reply));
    close(fds[0]);
    return r;
}

/* TC-4-20: 머리 도중 타임아웃 → 응답 없이 TIMED_OUT(D-53). */
static void slow_timeout_mid_head(void)
{
    int err;
    double secs;
    size_t reply_len;
    http_conn_result r = serve_open(S("GET / HTTP/1.1\r\nHo"), &err, &secs, &reply_len);
    check("TC-4-20 머리 도중 타임아웃 TIMED_OUT, 응답 0바이트, 약 5초",
          r == HTTP_CONN_TIMED_OUT && reply_len == 0 && secs >= 4.5 && secs < 6.5, "결과·응답·시간이 다르다");
}

/* TC-4-20: 버릴 본문 도중 타임아웃 → 응답 없이 TIMED_OUT. */
static void slow_timeout_mid_discard(void)
{
    int err;
    double secs;
    size_t reply_len;
    http_conn_result r = serve_open(S("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc"), &err, &secs, &reply_len);
    check("TC-4-20 버릴 본문 도중 타임아웃 TIMED_OUT, 응답 0바이트, 약 5초",
          r == HTTP_CONN_TIMED_OUT && reply_len == 0 && secs >= 4.5 && secs < 6.5, "결과·응답·시간이 다르다");
}

/* TC-4-30 (N1, P3): /echo 머리를 보낸 뒤 본문 도중 타임아웃 → TIMED_OUT, 머리와 받은 본문(abc)만 나간다(D-53 3행). */
static void slow_echo_timeout_mid_body(void)
{
    static char reply[4096];
    struct timespec start;
    int fds[2];
    double secs;
    size_t reply_len;
    http_conn_result r;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], S("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc"));
    clock_gettime(CLOCK_MONOTONIC, &start);
    r = drive(fds[1], root); /* 쓰기 방향은 열어 둔다 */
    secs = elapsed_since(&start);
    close(fds[1]);
    reply_len = read_all(fds[0], reply, sizeof(reply));
    close(fds[0]);
    check("TC-4-30 /echo 본문 도중 타임아웃 TIMED_OUT, 약 5초", r == HTTP_CONN_TIMED_OUT && secs >= 4.5 && secs < 6.5,
          "결과·시간이 다르다");
    check("TC-4-30 머리(Content-Length: 10) + abc만 나감",
          starts_with(reply, "HTTP/1.1 200 OK\r\n") && find_in(reply, reply_len, "\r\nContent-Length: 10\r\n") != NPOS &&
              find_in(reply, reply_len, "\r\n\r\nabc") + 7 == reply_len,
          reply);
}

/* D-53 1행: 유지 응답 뒤 요청 사이 타임아웃은 DONE. */
static void slow_idle_after_response(void)
{
    int err;
    double secs;
    size_t reply_len;
    http_conn_result r = serve_open(S(GET_ROOT), &err, &secs, &reply_len);
    check("D-53 요청 사이 유휴 5초 → DONE, 응답 하나", r == HTTP_CONN_DONE && reply_len > 0 && secs >= 4.5 && secs < 6.5,
          "결과·시간이 다르다");
}

/* TC-4-29: 응답을 받아 가지 않는 클라이언트 → 쓰기 5초 막힘 → ERROR(EAGAIN), 파일 fd 누수 없음. */
static void slow_write_blocked(void)
{
    const char req[] = "GET /huge.bin HTTP/1.1\r\nHost: x\r\n\r\n";
    struct timespec start;
    int fds[2], err, before;
    double secs;
    char detail[64];
    http_conn_result r;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], req, sizeof(req) - 1);
    before = lowest_free_fd();
    clock_gettime(CLOCK_MONOTONIC, &start);
    errno = 0;
    r = drive(fds[1], root); /* fds[0]은 열어 둔 채 읽지 않는다 */
    err = errno;
    secs = elapsed_since(&start);
    check("TC-4-29 쓰기 막힘 ERROR, errno EAGAIN·EWOULDBLOCK",
          r == HTTP_CONN_ERROR && (err == EAGAIN || err == EWOULDBLOCK), strerror(err));
    /* Step 5(D-59): 마지막 쓰기 진행 뒤 5초에 끝난다. 허용 범위는 Step 4 값을 그대로 확인만 한다(TC-5-07). */
    snprintf(detail, sizeof(detail), "%.2f초", secs);
    check("TC-4-29 4.5초 이상 걸리고 alarm(15) 전에 돌아옴", secs >= 4.5 && secs < 12.0, detail);
    check("TC-4-29 파일 fd 누수 없음", lowest_free_fd() == before, "열린 fd가 남았다");
    close(fds[0]);
    close(fds[1]);
}

/* fn을 자식 프로세스에서 돌린다. 자식은 자기 alarm을 걸고 실패가 있으면 1로 끝난다. */
static pid_t start_slow(void (*fn)(void))
{
    pid_t pid;
    fflush(stdout);
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        alarm(15);
        failures = 0;
        fn();
        fflush(stdout);
        _exit(failures > 0 ? 1 : 0);
    }
    return pid;
}

static void run_slow_tests(void)
{
    void (*fns[])(void) = {slow_timeout_mid_head, slow_timeout_mid_discard, slow_idle_after_response,
                           slow_write_blocked, slow_echo_timeout_mid_body};
    pid_t pids[sizeof(fns) / sizeof(fns[0])];
    size_t i;

    alarm(20); /* 자식들이 함께 기다린다(각각 약 5초) */
    for (i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        pids[i] = start_slow(fns[i]);
    }
    for (i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        int status;
        setup_or_die(waitpid(pids[i], &status, 0) == pids[i], "waitpid");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("  FAIL 느린 시험 %zu번 (자식 종료 상태 %d)\n", i + 1, status);
            failures++;
        }
    }
}

int main(void)
{
    alarm(10); /* 서버가 멈추면 끝없이 기다리지 않게 한다 */
    signal(SIGPIPE, SIG_IGN); /* D-05: 서버와 같은 조건 */
    printf("test_http_conn (TC-2-07, TC-3-08, TC-3-10, TC-4-10·19·20·22·24~36, TC-5-09·10)\n");
    build_root();
    test_split_request();
    test_close_mid_head();
    test_status_mapping();
    test_write_failure();
    test_big_file();
    test_close_mid_body();
    test_two_requests_at_once();
    test_expect_continue();
    test_keep_alive_rules();
    test_trailing_crlf();
    test_big_echo_then_next();
    test_echo_boundaries();
    test_keep_or_close_after_error();
    test_get_with_body();
    test_close_mid_discard();
    test_echo_peer_closed_mid_body();
    test_econnreset();
    test_continue_write_failure();
    test_leftover_partial_head();
    test_leftover_head_and_partial_body();
    test_head_exactly_8k();
    test_buffered_work();
    test_deadlines();
    run_slow_tests();
    remove_root();

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
