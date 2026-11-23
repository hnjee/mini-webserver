/* Step 2 단위 테스트: http_conn_serve를 socketpair로 시험한다(TC-2-07과 D-19 보조 검사). */
#define _POSIX_C_SOURCE 200809L

#include "../src/http_conn.h"
#include "../src/http_parser.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

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
    r = http_conn_serve(fds[1]);
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
    pid_t pid;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        http_conn_result r;
        close(fds[0]);
        r = http_conn_serve(fds[1]);
        close(fds[1]);
        _exit(r == HTTP_CONN_RESPONDED ? 0 : r == HTTP_CONN_PEER_CLOSED ? 1 : 2);
    }
    close(fds[1]);
    wrote = try_write_all(fds[0], "GET / HTTP/1.1\r\n", 16) == 0;
    nanosleep(&gap, NULL);
    wrote = wrote && try_write_all(fds[0], "Host: x\r\n\r\n", 11) == 0;
    check("TC-2-07 두 조각 모두 쓰기 성공", wrote, "서버가 요청 도중 닫았다");
    read_all(fds[0], reply, sizeof(reply));
    close(fds[0]);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    check("TC-2-07 나눠 쓴 요청에 200", starts_with(reply, "HTTP/1.1 200 OK\r\n"), "200 응답이 아니다");
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
    r = http_conn_serve(fds[1]);
    close(fds[1]);
    n = read_all(fds[0], reply, sizeof(reply));
    close(fds[0]);
    check("TC-2-07 머리 도중 닫기 PEER_CLOSED", r == HTTP_CONN_PEER_CLOSED, "PEER_CLOSED가 아니다");
    check("TC-2-07 머리 도중 닫기 응답 없음", n == 0, "응답 바이트가 있다");
}

/* D-19 보조: 결과별 상태 코드, 8192바이트로 버퍼가 꽉 찬 경우(0바이트 read 없이 400), 쓰기 실패. */
static void test_status_mapping(void)
{
    static const struct {
        const char *name;
        const char *req;
        const char *line;
    } cases[] = {
        {"D-19 OK → 200", "GET / HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 200 OK\r\n"},
        {"D-19 POST 본문은 읽지 않고 200", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc",
         "HTTP/1.1 200 OK\r\n"},
        {"D-19 BAD_REQUEST → 400", "GET / HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request\r\n"},
        {"D-19 NOT_IMPLEMENTED → 501", "BREW / HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 501 Not Implemented\r\n"},
        {"D-19 VERSION_NOT_SUPPORTED → 505", "GET / HTTP/2.0\r\nHost: x\r\n\r\n",
         "HTTP/1.1 505 HTTP Version Not Supported\r\n"},
    };
    static char reply[4096], full[HTTP_REQUEST_HEAD_MAX];
    size_t i;
    http_conn_result r;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        r = serve_prewritten(cases[i].req, strlen(cases[i].req), reply, sizeof(reply));
        check(cases[i].name, r == HTTP_CONN_RESPONDED && starts_with(reply, cases[i].line), reply);
    }

    memset(full, 'a', sizeof(full));
    r = serve_prewritten(full, sizeof(full), reply, sizeof(reply));
    check("D-19 종료 없이 8192바이트 → 400 (0바이트 read 없음)",
          r == HTTP_CONN_RESPONDED && starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"),
          r == HTTP_CONN_PEER_CLOSED ? "PEER_CLOSED로 끝났다" : "400 응답이 아니다");
}

static void test_write_failure(void)
{
    const char req[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    int fds[2], err;
    http_conn_result r;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], req, sizeof(req) - 1);
    close(fds[0]); /* 요청은 받을 수 있지만 응답 쓰기는 EPIPE가 된다 */
    errno = 0;
    r = http_conn_serve(fds[1]);
    err = errno;
    close(fds[1]);
    check("D-19 응답 쓰기 실패 ERROR, errno EPIPE", r == HTTP_CONN_ERROR && err == EPIPE, strerror(err));
}

int main(void)
{
    alarm(10); /* 서버가 멈추면 끝없이 기다리지 않게 한다 */
    signal(SIGPIPE, SIG_IGN); /* D-05: 서버와 같은 조건 */
    printf("test_http_conn (TC-2-07)\n");
    test_split_request();
    test_close_mid_head();
    test_status_mapping();
    test_write_failure();

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
