/* Step 2~5 단위 테스트: http_response를 socketpair로 시험한다(TC-2-05, TC-2-06, TC-3-09, TC-4-15, TC-5-07). */
#define _POSIX_C_SOURCE 200809L

#include "../src/http_response.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static const http_response_extra close_extra = {NULL, NULL, HTTP_CONNECTION_CLOSE};

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
    rc = http_response_send(fds[1], status, ctype, body, body_len, &close_extra);
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

    rc = send_round_trip(418, "text/plain", "x", 1, reply, sizeof(reply), &reply_len, &err);
    check("TC-2-05 지원하지 않는 상태 -1(EINVAL)", rc == -1 && err == EINVAL, "반환값·errno가 다르다");
    check("TC-2-05 지원하지 않는 상태는 아무것도 쓰지 않음", reply_len == 0, "바이트가 쓰였다");
}

static void test_write_failure(void)
{
    int fds[2], rc, err;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    close(fds[0]);
    errno = 0;
    rc = http_response_send(fds[1], 200, "text/plain", "x", 1, NULL);
    err = errno;
    check("TC-2-05 상대가 닫으면 -1, errno EPIPE", rc == -1 && err == EPIPE, strerror(err));
    close(fds[1]);
}

/* TC-2-06: 소켓 버퍼보다 큰 본문. 다른 프로세스가 보내고 이 프로세스가 읽는다. 전 바이트 도착만 확인한다(D-20). */
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
        _exit(http_response_send(fds[1], 200, "application/octet-stream", body, body_len, &close_extra) == 0 ? 0 : 1);
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

/* fd 쌍을 만들어 fn이 server 쪽에 쓰게 하고, 닫은 뒤 client 쪽에서 EOF까지 읽는다. */
typedef int (*writer_fn)(int fd, const void *arg);

static int write_round_trip(writer_fn fn, const void *arg, char *reply, size_t cap, size_t *reply_len, int *err)
{
    int fds[2], rc;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    errno = 0;
    rc = fn(fds[1], arg);
    *err = errno;
    close(fds[1]);
    *reply_len = read_all(fds[0], reply, cap);
    close(fds[0]);
    return rc;
}

typedef struct {
    int status;
    const char *ctype;
    size_t length;
    http_response_extra extra;
} call_args;

static int call_send_head(int fd, const void *arg)
{
    const call_args *a = arg;
    return http_response_send_head(fd, a->status, a->ctype, a->length, &a->extra);
}

static int call_send_status(int fd, const void *arg)
{
    const call_args *a = arg;
    return http_response_send_status(fd, a->status, &a->extra);
}

static int call_send(int fd, const void *arg)
{
    const call_args *a = arg;
    return http_response_send(fd, a->status, a->ctype, "x", 1, &a->extra);
}

/* TC-3-09: 머리만 쓰고 Content-Length는 인자 그대로, 빈 줄 뒤 바이트 없음, Location 없음. */
static void test_send_head(void)
{
    static char reply[4096];
    call_args a = {200, "text/html; charset=utf-8", 12345, {NULL, NULL, HTTP_CONNECTION_CLOSE}};
    size_t reply_len, head_len;
    int rc, err;

    rc = write_round_trip(call_send_head, &a, reply, sizeof(reply), &reply_len, &err);
    check("TC-3-09 send_head 200 성공", rc == 0, strerror(err));
    head_len = find(reply, reply_len, "\r\n\r\n");
    check("TC-3-09 send_head 빈 줄 뒤 바이트 없음", head_len != (size_t)-1 && head_len + 4 == reply_len,
          "본문 바이트가 있다");
    check("TC-3-09 send_head 상태 줄", reply_len > 17 && memcmp(reply, "HTTP/1.1 200 OK\r\n", 17) == 0, "다르다");
    check("TC-3-09 send_head Content-Length = 인자", find(reply, reply_len, "\r\nContent-Length: 12345\r\n") !=
                                                          (size_t)-1,
          "다르다");
    check("TC-3-09 send_head Content-Type", find(reply, reply_len, "\r\nContent-Type: text/html; charset=utf-8\r\n") !=
                                                (size_t)-1,
          "다르다");
    check("TC-3-09 send_head Location 없음", find(reply, reply_len, "Location") == (size_t)-1, "있다");
    check("TC-3-09 send_head Connection: close", find(reply, reply_len, "\r\nConnection: close\r\n") != (size_t)-1,
          "없다");
    check("TC-3-09 send_head Date 형식", date_line_ok(reply, reply_len), "IMF-fixdate가 아니다");
}

/* 상태 응답의 본문 "<코드> <이유 문구>\n"과 Content-Length. */
static void check_status_reply(const char *name, int status, const char *line, const char *location,
                               const char *reply, size_t reply_len)
{
    char label[160], expected[PATH_MAX + 64], body[64];
    size_t head_len = find(reply, reply_len, "\r\n\r\n");
    size_t body_len = strlen(line) - 9 - 2; /* "HTTP/1.1 "와 끝 "\r\n"을 뺀 "<코드> <이유 문구>" */

    memcpy(body, line + 9, body_len);
    body[body_len++] = '\n';
    body[body_len] = '\0';
    snprintf(label, sizeof(label), "%s 머리 끝 있음", name);
    check(label, head_len != (size_t)-1, "\\r\\n\\r\\n이 없다");
    if (head_len == (size_t)-1) {
        return;
    }
    head_len += 4;
    snprintf(label, sizeof(label), "%s 상태 줄", name);
    check(label, memcmp(reply, line, strlen(line)) == 0, "다르다");
    snprintf(label, sizeof(label), "%s Content-Length = 본문 \"%d ...\" 길이 %zu", name, status, body_len);
    snprintf(expected, sizeof(expected), "\r\nContent-Length: %zu\r\n", body_len);
    check(label, find(reply, head_len, expected) != (size_t)-1, "다르다");
    snprintf(label, sizeof(label), "%s Content-Type text/plain", name);
    check(label, find(reply, head_len, "\r\nContent-Type: text/plain; charset=utf-8\r\n") != (size_t)-1, "다르다");
    snprintf(label, sizeof(label), "%s 본문 \"<코드> <이유 문구>\\n\"", name);
    check(label, reply_len - head_len == body_len && memcmp(reply + head_len, body, body_len) == 0, "다르다");
    if (location != NULL) {
        snprintf(label, sizeof(label), "%s Location이 Connection 앞 한 줄", name);
        snprintf(expected, sizeof(expected), "\r\nLocation: %s\r\nConnection: close\r\n", location);
        check(label, find(reply, head_len, expected) != (size_t)-1, "다르다");
    } else {
        snprintf(label, sizeof(label), "%s Location 없음", name);
        check(label, find(reply, head_len, "Location") == (size_t)-1, "있다");
    }
    snprintf(label, sizeof(label), "%s Date 형식", name);
    check(label, date_line_ok(reply, head_len), "IMF-fixdate가 아니다");
}

static void test_send_status(void)
{
    static const struct {
        int status;
        const char *line;
    } cases[] = {
        {301, "HTTP/1.1 301 Moved Permanently\r\n"},
        {400, "HTTP/1.1 400 Bad Request\r\n"},
        {403, "HTTP/1.1 403 Forbidden\r\n"},
        {404, "HTTP/1.1 404 Not Found\r\n"},
        {500, "HTTP/1.1 500 Internal Server Error\r\n"},
        {501, "HTTP/1.1 501 Not Implemented\r\n"},
        {505, "HTTP/1.1 505 HTTP Version Not Supported\r\n"},
    };
    static char reply[2 * PATH_MAX + 1024], long_location[PATH_MAX + 1];
    size_t i, reply_len;
    int rc, err;
    char name[96];

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        call_args a = {cases[i].status, NULL, 0, {cases[i].status == 301 ? "/sub/" : NULL, NULL, HTTP_CONNECTION_CLOSE}};
        snprintf(name, sizeof(name), "TC-3-09 send_status %d", cases[i].status);
        rc = write_round_trip(call_send_status, &a, reply, sizeof(reply), &reply_len, &err);
        check(name, rc == 0, strerror(err));
        check_status_reply(name, cases[i].status, cases[i].line, a.extra.location, reply, reply_len);
    }

    /* PATH_MAX바이트 Location(D-27 머리 버퍼 512 + PATH_MAX) */
    long_location[0] = '/';
    memset(long_location + 1, 'a', PATH_MAX - 2);
    long_location[PATH_MAX - 1] = '/';
    long_location[PATH_MAX] = '\0';
    {
        call_args a = {301, NULL, 0, {long_location, NULL, HTTP_CONNECTION_CLOSE}};
        rc = write_round_trip(call_send_status, &a, reply, sizeof(reply), &reply_len, &err);
        check("TC-3-09 send_status 301 PATH_MAX바이트 Location 성공", rc == 0, strerror(err));
        check_status_reply("TC-3-09 PATH_MAX Location", 301, "HTTP/1.1 301 Moved Permanently\r\n", long_location, reply,
                           reply_len);
    }
}

/* TC-3-09: 계약 위반은 아무것도 쓰지 않고 -1(EINVAL). */
static void test_invalid_arguments(void)
{
    static const struct {
        const char *name;
        writer_fn fn;
        call_args a;
    } cases[] = {
        {"send_status 400인데 location", call_send_status, {400, NULL, 0, {"/x/", NULL, 0}}},
        {"send_status 301인데 NULL", call_send_status, {301, NULL, 0, {NULL, NULL, 0}}},
        {"send_status 418", call_send_status, {418, NULL, 0, {NULL, NULL, 0}}},
        {"send_status 200", call_send_status, {200, NULL, 0, {NULL, NULL, 0}}},
        {"send_head 200인데 location", call_send_head, {200, "text/plain", 1, {"/x/", NULL, 0}}},
        {"send_head 301인데 NULL", call_send_head, {301, "text/plain", 1, {NULL, NULL, 0}}},
        {"send_head 418", call_send_head, {418, "text/plain", 1, {NULL, NULL, 0}}},
        {"send 301 (location을 줄 수 없다)", call_send, {301, "text/plain", 1, {NULL, NULL, 0}}},
        /* TC-4-15: Allow 규칙(D-52) */
        {"TC-4-15 send_status 405인데 allow NULL", call_send_status, {405, NULL, 0, {NULL, NULL, 0}}},
        {"TC-4-15 send_status 404인데 allow", call_send_status, {404, NULL, 0, {NULL, "GET", 0}}},
        {"TC-4-15 send_status 301에 allow", call_send_status, {301, NULL, 0, {"/x/", "GET", 0}}},
        {"TC-4-15 send_status 405에 location", call_send_status, {405, NULL, 0, {"/x/", "GET", 0}}},
        {"TC-4-15 send_head 405인데 allow NULL", call_send_head, {405, "text/plain", 1, {NULL, NULL, 0}}},
        {"TC-4-15 send_head 200인데 allow", call_send_head, {200, "text/plain", 1, {NULL, "GET", 0}}},
        {"TC-4-15 send에 location", call_send, {200, "text/plain", 1, {"/x/", NULL, 0}}},
        {"TC-4-15 send에 allow (405)", call_send, {405, "text/plain", 1, {NULL, "GET", 0}}},
        {"TC-4-15 send 405 (allow를 줄 수 없다)", call_send, {405, "text/plain", 1, {NULL, NULL, 0}}},
        /* TC-4-15: 100은 전용 함수만 쓴다 */
        {"TC-4-15 send_head 100", call_send_head, {100, "text/plain", 0, {NULL, NULL, 0}}},
        {"TC-4-15 send_status 100", call_send_status, {100, NULL, 0, {NULL, NULL, 0}}},
        {"TC-4-15 send 100", call_send, {100, "text/plain", 1, {NULL, NULL, 0}}},
    };
    static char reply[4096];
    size_t i, reply_len;
    int rc, err;
    char name[128];

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rc = write_round_trip(cases[i].fn, &cases[i].a, reply, sizeof(reply), &reply_len, &err);
        snprintf(name, sizeof(name), "TC-3-09 %s → -1(EINVAL), 0바이트", cases[i].name);
        check(name, rc == -1 && err == EINVAL && reply_len == 0, reply_len ? "바이트가 쓰였다" : strerror(err));
    }
}

/* TC-4-15: 새 상태·Allow·머리 순서. */
static void test_step4_statuses(void)
{
    static const struct {
        int status;
        const char *line;
        const char *allow;
    } cases[] = {
        {405, "HTTP/1.1 405 Method Not Allowed\r\n", "GET"},
        {413, "HTTP/1.1 413 Content Too Large\r\n", NULL},
        {414, "HTTP/1.1 414 URI Too Long\r\n", NULL},
        {431, "HTTP/1.1 431 Request Header Fields Too Large\r\n", NULL},
    };
    static char reply[4096];
    size_t i, reply_len;
    int rc, err;
    char name[96];

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        call_args a = {cases[i].status, NULL, 0, {NULL, cases[i].allow, HTTP_CONNECTION_CLOSE}};
        snprintf(name, sizeof(name), "TC-4-15 send_status %d", cases[i].status);
        rc = write_round_trip(call_send_status, &a, reply, sizeof(reply), &reply_len, &err);
        check(name, rc == 0, strerror(err));
        check_status_reply(name, cases[i].status, cases[i].line, NULL, reply, reply_len);
    }
    {
        call_args a = {405, NULL, 0, {NULL, "POST", HTTP_CONNECTION_NONE}};
        rc = write_round_trip(call_send_status, &a, reply, sizeof(reply), &reply_len, &err);
        check("TC-4-15 405 Allow: POST 성공", rc == 0, strerror(err));
        check("TC-4-15 405 Allow는 Content-Length 뒤 마지막 머리",
              find(reply, reply_len, "\r\nContent-Length: 23\r\nAllow: POST\r\n\r\n") != (size_t)-1, "순서가 다르다");
    }
    {
        size_t date, ctype, clen, allow, conn;
        call_args a = {405, NULL, 0, {NULL, "GET", HTTP_CONNECTION_KEEP_ALIVE}};
        rc = write_round_trip(call_send_status, &a, reply, sizeof(reply), &reply_len, &err);
        date = find(reply, reply_len, "\r\nDate: ");
        ctype = find(reply, reply_len, "\r\nContent-Type: ");
        clen = find(reply, reply_len, "\r\nContent-Length: ");
        allow = find(reply, reply_len, "\r\nAllow: GET\r\n");
        conn = find(reply, reply_len, "\r\nConnection: keep-alive\r\n\r\n");
        check("TC-4-15 머리 순서 Date, Content-Type, Content-Length, Allow, Connection",
              rc == 0 && date < ctype && ctype < clen && clen < allow && allow < conn && conn != (size_t)-1,
              "순서가 다르다");
        check("TC-4-15 Date가 상태 줄 바로 다음", date == strlen("HTTP/1.1 405 Method Not Allowed"), "다르다");
    }
}

/* TC-4-15: Connection 세 값(D-52). */
static void test_connection_values(void)
{
    static const struct {
        const char *name;
        http_connection connection;
        const char *want; /* NULL이면 Connection 머리가 없어야 한다 */
    } cases[] = {
        {"TC-4-15 Connection 넣지 않음", HTTP_CONNECTION_NONE, NULL},
        {"TC-4-15 Connection: close", HTTP_CONNECTION_CLOSE, "\r\nContent-Length: 1\r\nConnection: close\r\n\r\n"},
        {"TC-4-15 Connection: keep-alive", HTTP_CONNECTION_KEEP_ALIVE,
         "\r\nContent-Length: 1\r\nConnection: keep-alive\r\n\r\n"},
    };
    static char reply[4096];
    size_t i, reply_len;
    int rc, err;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        call_args a = {200, "text/plain", 1, {NULL, NULL, cases[i].connection}};
        rc = write_round_trip(call_send, &a, reply, sizeof(reply), &reply_len, &err);
        if (cases[i].want == NULL) {
            check(cases[i].name, rc == 0 && find(reply, reply_len, "Connection") == (size_t)-1 &&
                                     find(reply, reply_len, "\r\nContent-Length: 1\r\n\r\nx") != (size_t)-1,
                  "Connection 머리가 있다");
        } else {
            check(cases[i].name, rc == 0 && find(reply, reply_len, cases[i].want) != (size_t)-1, "다르다");
        }
    }
    {
        int fds[2];
        setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
        errno = 0;
        rc = http_response_send_head(fds[1], 200, "text/plain", 0, NULL);
        err = errno;
        close(fds[1]);
        reply_len = read_all(fds[0], reply, sizeof(reply));
        close(fds[0]);
        check("TC-4-15 추가 머리 NULL = 모두 없음",
              rc == 0 && find(reply, reply_len, "Connection") == (size_t)-1 &&
                  find(reply, reply_len, "Location") == (size_t)-1 && find(reply, reply_len, "Allow") == (size_t)-1,
              strerror(err));
    }
}

static int call_continue(int fd, const void *arg)
{
    (void)arg;
    return http_response_send_continue(fd);
}

/* TC-4-15: 100 전용 함수는 상태 줄과 빈 줄만 쓴다. */
static void test_continue(void)
{
    static const char want[] = "HTTP/1.1 100 Continue\r\n\r\n";
    static char reply[256];
    size_t reply_len;
    int fds[2], rc, err;

    rc = write_round_trip(call_continue, NULL, reply, sizeof(reply), &reply_len, &err);
    check("TC-4-15 100 Continue 정확한 바이트", rc == 0 && reply_len == sizeof(want) - 1 &&
                                                  memcmp(reply, want, reply_len) == 0,
          "다르다");
    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    close(fds[0]);
    errno = 0;
    rc = http_response_send_continue(fds[1]);
    err = errno;
    close(fds[1]);
    check("TC-4-15 100 쓰기 실패 -1, errno EPIPE", rc == -1 && err == EPIPE, strerror(err));
}

/* ---- TC-5-07: 버퍼에 쓰는 함수가 send_*와 같은 바이트를 만든다(D-58) ---- */

typedef struct {
    const char *name;
    int kind; /* 0 head, 1 status, 2 continue */
    call_args a;
} format_case;

/* 같은 인자로 format_*과 send_*를 부른다. Date가 초 경계를 넘으면 다를 수 있어 세 번까지 다시 해 본다. */
static void compare_format_send(const format_case *c)
{
    static char sent[2 * PATH_MAX + 1024], formatted[2 * PATH_MAX + 1024];
    static const writer_fn senders[] = {call_send_head, call_send_status, call_continue};
    char label[160];
    size_t sent_len = 0;
    int rc_send = 0, rc_fmt = 0, err_send = 0, err_fmt = 0, attempt, same = 0;

    for (attempt = 0; attempt < 3 && !same; attempt++) {
        memset(formatted, '#', sizeof(formatted));
        errno = 0;
        rc_fmt = c->kind == 0   ? http_response_format_head(formatted, sizeof(formatted), c->a.status, c->a.ctype,
                                                            c->a.length, &c->a.extra)
                 : c->kind == 1 ? http_response_format_status(formatted, sizeof(formatted), c->a.status, &c->a.extra)
                                : http_response_format_continue(formatted, sizeof(formatted));
        err_fmt = errno;
        rc_send = write_round_trip(senders[c->kind], &c->a, sent, sizeof(sent), &sent_len, &err_send);
        same = rc_send == 0 ? rc_fmt >= 0 && (size_t)rc_fmt == sent_len && memcmp(formatted, sent, sent_len) == 0
                            : rc_fmt == -1 && err_fmt == err_send && sent_len == 0;
    }
    snprintf(label, sizeof(label), "TC-5-07 format = send 바이트: %s", c->name);
    check(label, same, rc_fmt < 0 ? strerror(err_fmt) : "바이트·길이·반환값이 다르다");
}

static void test_format_matches_send(void)
{
    static char long_location[PATH_MAX + 1];
    static const format_case base[] = {
        {"head 200 close", 0, {200, "text/html; charset=utf-8", 12345, {NULL, NULL, HTTP_CONNECTION_CLOSE}}},
        {"head 200 /echo 유지(Connection 없음)", 0, {200, "application/octet-stream", 5, {NULL, NULL, HTTP_CONNECTION_NONE}}},
        {"head 200 1.0 keep-alive", 0, {200, "text/css", 0, {NULL, NULL, HTTP_CONNECTION_KEEP_ALIVE}}},
        {"status 404 유지", 1, {404, NULL, 0, {NULL, NULL, HTTP_CONNECTION_NONE}}},
        {"status 301 Location", 1, {301, NULL, 0, {"/sub/", NULL, HTTP_CONNECTION_NONE}}},
        {"status 405 Allow: GET", 1, {405, NULL, 0, {NULL, "GET", HTTP_CONNECTION_KEEP_ALIVE}}},
        {"status 413 close", 1, {413, NULL, 0, {NULL, NULL, HTTP_CONNECTION_CLOSE}}},
        {"status 431 close", 1, {431, NULL, 0, {NULL, NULL, HTTP_CONNECTION_CLOSE}}},
        {"100 Continue", 2, {100, NULL, 0, {NULL, NULL, 0}}},
        {"계약 위반 status 200 → -1", 1, {200, NULL, 0, {NULL, NULL, 0}}},
        {"계약 위반 head 405 allow NULL → -1", 0, {405, "text/plain", 1, {NULL, NULL, 0}}},
        {"계약 위반 head 418 → -1", 0, {418, "text/plain", 1, {NULL, NULL, 0}}},
    };
    char small[16];
    size_t i;
    int rc, err;

    for (i = 0; i < sizeof(base) / sizeof(base[0]); i++) {
        compare_format_send(&base[i]);
    }
    long_location[0] = '/';
    memset(long_location + 1, 'a', PATH_MAX - 2);
    long_location[PATH_MAX - 1] = '/';
    long_location[PATH_MAX] = '\0';
    {
        const format_case c = {"status 301 PATH_MAX Location", 1, {301, NULL, 0, {long_location, NULL, HTTP_CONNECTION_CLOSE}}};
        compare_format_send(&c);
    }
    errno = 0;
    rc = http_response_format_status(small, sizeof(small), 404, NULL);
    err = errno;
    check("TC-5-07 format 버퍼가 모자라면 -1(EINVAL)", rc == -1 && err == EINVAL, "반환값·errno가 다르다");
    errno = 0;
    rc = http_response_format_continue(small, 10);
    err = errno;
    check("TC-5-07 format_continue 버퍼가 모자라면 -1(EINVAL)", rc == -1 && err == EINVAL, "반환값·errno가 다르다");
}

int main(void)
{
    alarm(10); /* 멈추면 끝없이 기다리지 않게 한다 */
    signal(SIGPIPE, SIG_IGN); /* D-05: 서버와 같은 조건 */
    printf("test_http_response (TC-2-05, TC-2-06, TC-3-09, TC-4-15, TC-5-07)\n");
    test_format_matches_send();
    test_statuses();
    test_write_failure();
    test_large_body();
    test_send_head();
    test_send_status();
    test_invalid_arguments();
    test_step4_statuses();
    test_connection_values();
    test_continue();

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
