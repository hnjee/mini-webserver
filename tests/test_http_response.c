/* Step 2 단위 테스트: http_response_send를 socketpair로 시험한다(TC-2-05, TC-2-06). */
#define _POSIX_C_SOURCE 200809L

#include "../src/http_response.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

/* 테스트 쪽 준비 실패는 제품 실패와 구분해 즉시 중단한다. */
static void setup_or_die(int ok, const char *what)
{
    if (!ok) {
        printf("  SETUP ERROR %s: %s\n", what, strerror(errno));
        exit(2);
    }
}

/* EOF까지 읽는다. 읽은 길이를 돌려준다. cap을 넘으면 준비 실패로 본다. */
static size_t read_all(int fd, char *out, size_t cap)
{
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, out + total, cap - total);
        setup_or_die(n >= 0, "테스트 read");
        if (n == 0) {
            return total;
        }
        total += (size_t)n;
        setup_or_die(total < cap, "응답이 테스트 버퍼보다 크다");
    }
}

static size_t find(const char *hay, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    for (i = 0; i + n <= len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

/* "Date: Sun, 06 Nov 1994 08:49:37 GMT" 형식(IMF-fixdate)인가. */
static int date_line_ok(const char *head, size_t head_len)
{
    static const char *days = "Mon Tue Wed Thu Fri Sat Sun ";
    static const char *months = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec ";
    const char *pattern = "AAA, 00 AAA 0000 00:00:00 GMT\r\n"; /* A: 영문자, 0: 숫자, 나머지는 그대로 */
    size_t at = find(head, head_len, "\r\nDate: "), i;
    const char *d;
    char word[5];

    if (at == (size_t)-1 || at + 8 + strlen(pattern) > head_len) {
        return 0;
    }
    d = head + at + 8;
    for (i = 0; pattern[i] != '\0'; i++) {
        char p = pattern[i];
        if ((p == 'A' && !isalpha((unsigned char)d[i])) || (p == '0' && !isdigit((unsigned char)d[i])) ||
            (p != 'A' && p != '0' && p != d[i])) {
            return 0;
        }
    }
    snprintf(word, sizeof(word), "%.3s ", d);
    if (strstr(days, word) == NULL) {
        return 0;
    }
    snprintf(word, sizeof(word), "%.3s ", d + 8);
    return strstr(months, word) != NULL;
}

/* 응답 바이트가 상태 줄·헤더·본문 규칙을 지키는지 확인한다. name은 출력용 접두어. */
static void check_response(const char *name, const char *reply, size_t reply_len,
                           const char *status_line, const char *content_type,
                           const char *body, size_t body_len)
{
    char label[128], expected[128];
    size_t head_len = find(reply, reply_len, "\r\n\r\n");

    snprintf(label, sizeof(label), "%s 머리 끝 있음", name);
    check(label, head_len != (size_t)-1, "\\r\\n\\r\\n이 없다");
    if (head_len == (size_t)-1) {
        return;
    }
    head_len += 4;
    snprintf(label, sizeof(label), "%s 상태 줄", name);
    check(label, reply_len >= strlen(status_line) && memcmp(reply, status_line, strlen(status_line)) == 0,
          "상태 줄이 다르다");
    snprintf(label, sizeof(label), "%s Content-Length = 실제 본문 길이", name);
    snprintf(expected, sizeof(expected), "\r\nContent-Length: %zu\r\n", reply_len - head_len);
    check(label, find(reply, head_len, expected) != (size_t)-1 && reply_len - head_len == body_len,
          "길이가 다르다");
    snprintf(label, sizeof(label), "%s 본문 바이트 그대로", name);
    check(label, reply_len - head_len == body_len && memcmp(reply + head_len, body, body_len) == 0,
          "본문이 다르다");
    snprintf(label, sizeof(label), "%s Content-Type", name);
    snprintf(expected, sizeof(expected), "\r\nContent-Type: %s\r\n", content_type);
    check(label, find(reply, head_len, expected) != (size_t)-1, "Content-Type이 다르다");
    snprintf(label, sizeof(label), "%s Connection: close", name);
    check(label, find(reply, head_len, "\r\nConnection: close\r\n") != (size_t)-1, "없다");
    snprintf(label, sizeof(label), "%s Date 형식", name);
    check(label, date_line_ok(reply, head_len), "IMF-fixdate가 아니다");
}

/* server 쪽에 응답을 쓰고 닫은 뒤 client 쪽에서 EOF까지 읽는다. send의 반환값과 errno를 돌려준다. */
static int send_round_trip(int status, const char *ctype, const char *body, size_t body_len,
                           char *reply, size_t cap, size_t *reply_len, int *err)
{
    int fds[2], rc;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    errno = 0;
    rc = http_response_send(fds[1], status, ctype, body, body_len);
    *err = errno;
    close(fds[1]);
    *reply_len = read_all(fds[0], reply, cap);
    close(fds[0]);
    return rc;
}

static void test_statuses(void)
{
    static const struct {
        int status;
        const char *line;
    } cases[] = {
        {200, "HTTP/1.1 200 OK\r\n"},
        {400, "HTTP/1.1 400 Bad Request\r\n"},
        {501, "HTTP/1.1 501 Not Implemented\r\n"},
        {505, "HTTP/1.1 505 HTTP Version Not Supported\r\n"},
    };
    static char reply[4096];
    const char body[] = "hello\0world\n"; /* NUL 포함 본문 */
    size_t i, reply_len;
    int err, rc;
    char name[64];

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(name, sizeof(name), "TC-2-05 %d", cases[i].status);
        rc = send_round_trip(cases[i].status, "text/plain; charset=utf-8", body, sizeof(body) - 1,
                             reply, sizeof(reply), &reply_len, &err);
        check(name, rc == 0, strerror(err));
        check_response(name, reply, reply_len, cases[i].line, "text/plain; charset=utf-8", body,
                       sizeof(body) - 1);
    }

    rc = send_round_trip(200, "text/plain", "", 0, reply, sizeof(reply), &reply_len, &err);
    check("TC-2-05 빈 본문 성공", rc == 0, strerror(err));
    check_response("TC-2-05 빈 본문", reply, reply_len, "HTTP/1.1 200 OK\r\n", "text/plain", "", 0);

    rc = send_round_trip(404, "text/plain", "x", 1, reply, sizeof(reply), &reply_len, &err);
    check("TC-2-05 지원하지 않는 상태 -1(EINVAL)", rc == -1 && err == EINVAL, "반환값·errno가 다르다");
    check("TC-2-05 지원하지 않는 상태는 아무것도 쓰지 않음", reply_len == 0, "바이트가 쓰였다");
}

static void test_write_failure(void)
{
    int fds[2], rc, err;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    close(fds[0]);
    errno = 0;
    rc = http_response_send(fds[1], 200, "text/plain", "x", 1);
    err = errno;
    check("TC-2-05 상대가 닫으면 -1, errno EPIPE", rc == -1 && err == EPIPE, strerror(err));
    close(fds[1]);
}

/* TC-2-06: 소켓 버퍼보다 큰 본문. 다른 프로세스가 보내고 이 프로세스가 읽는다. 전 바이트 도착만 확인한다(D-23). */
static void test_large_body(void)
{
    const size_t body_len = 4u * 1024 * 1024;
    const size_t cap = body_len + 4096;
    char *body = malloc(body_len), *reply = malloc(cap);
    int fds[2], status;
    size_t i, reply_len;
    pid_t pid;

    setup_or_die(body != NULL && reply != NULL, "malloc");
    for (i = 0; i < body_len; i++) {
        body[i] = (char)(i * 131 + 17);
    }
    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        close(fds[0]);
        _exit(http_response_send(fds[1], 200, "application/octet-stream", body, body_len) == 0 ? 0 : 1);
    }
    close(fds[1]);
    reply_len = read_all(fds[0], reply, cap);
    close(fds[0]);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    check("TC-2-06 보내는 프로세스 성공(0)", WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "자식이 실패했다");
    check_response("TC-2-06 4MB", reply, reply_len, "HTTP/1.1 200 OK\r\n", "application/octet-stream",
                   body, body_len);
    free(body);
    free(reply);
}

int main(void)
{
    alarm(10); /* 멈추면 끝없이 기다리지 않게 한다 */
    signal(SIGPIPE, SIG_IGN); /* D-05: 서버와 같은 조건 */
    printf("test_http_response (TC-2-05, TC-2-06)\n");
    test_statuses();
    test_write_failure();
    test_large_body();

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
