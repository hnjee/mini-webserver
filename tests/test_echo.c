/* Step 1 단위 테스트: echo 모듈을 socketpair로 시험한다(TC-1-07~10). */
#define _POSIX_C_SOURCE 200809L

#include "../src/echo.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static void write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        setup_or_die(n > 0, "테스트 write");
        data += n;
        len -= (size_t)n;
    }
}

/* EOF까지 읽는다. 읽은 길이를 돌려준다. */
static size_t read_all(int fd, char *out, size_t cap)
{
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, out + total, cap - total);
        if (n < 0 && errno == ECONNRESET) {
            return total; /* echo가 입력을 다 읽지 않고 끝나면 생긴다. 응답 비교에서 실패로 잡힌다 */
        }
        setup_or_die(n >= 0, "테스트 read");
        if (n == 0 || total + (size_t)n == cap) {
            return total + (size_t)n;
        }
        total += (size_t)n;
    }
}

/*
 * client에 data를 쓰고 SHUT_WR 한 뒤 server 쪽으로 echo_serve를 부른다.
 * server fd는 main처럼 호출한 쪽(테스트)이 닫고, client에서 EOF까지 응답을 읽는다.
 */
static echo_result echo_round_trip(const char *data, size_t len, char *reply, size_t cap,
                                   size_t *reply_len)
{
    int fds[2];
    echo_result result;

    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    write_all(fds[0], data, len);
    setup_or_die(shutdown(fds[0], SHUT_WR) == 0, "shutdown");
    result = echo_serve(fds[1]);
    close(fds[1]);
    *reply_len = read_all(fds[0], reply, cap);
    close(fds[0]);
    return result;
}

int main(void)
{
    static char reply[64 * 1024];
    size_t reply_len;
    echo_result result;

    alarm(10); /* echo가 멈추면 테스트가 끝없이 기다리지 않게 한다 */
    signal(SIGPIPE, SIG_IGN); /* D-05: 서버와 같은 조건 */
    printf("test_echo\n");

    {
        /* TC-1-07: 개행 없는 바이트와 0x00 */
        const char data[] = {'a', 'b', '\0', 'c', 'd', '\0', 'e'};
        result = echo_round_trip(data, sizeof(data), reply, sizeof(reply), &reply_len);
        check("TC-1-07 개행 없음·0x00 그대로", reply_len == sizeof(data) &&
              memcmp(reply, data, sizeof(data)) == 0, "응답이 다르다");
    }
    {
        /* TC-1-08: 상대가 쓰고 SHUT_WR → 응답 확인, 상대 종료로 반환 */
        const char data[] = "hello";
        result = echo_round_trip(data, 5, reply, sizeof(reply), &reply_len);
        check("TC-1-08 응답 그대로", reply_len == 5 && memcmp(reply, data, 5) == 0, "응답이 다르다");
        check("TC-1-08 상대 종료로 반환", result == ECHO_PEER_CLOSED, "ECHO_PEER_CLOSED가 아니다");
    }
    {
        /* TC-1-09: SIGPIPE 무시 상태에서 상대가 쓰고 close() → 쓰기 오류(EPIPE), 프로세스 유지 */
        int fds[2];
        setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
        write_all(fds[0], "bye", 3);
        close(fds[0]);
        errno = 0;
        result = echo_serve(fds[1]);
        check("TC-1-09 연결 오류로 반환", result == ECHO_ERROR, "ECHO_ERROR가 아니다");
        check("TC-1-09 errno 보존(EPIPE)", errno == EPIPE, strerror(errno));
        close(fds[1]);
    }
    {
        /* TC-1-10: 버퍼보다 큰 입력(4배 + 자투리). socketpair 기본 버퍼(약 200 KB) 안이다. */
        size_t len = 4 * ECHO_BUFFER_SIZE + 123;
        char *data = malloc(len);
        size_t i;
        setup_or_die(data != NULL, "malloc");
        for (i = 0; i < len; i++) {
            data[i] = (char)(i * 31 + 7);
        }
        result = echo_round_trip(data, len, reply, sizeof(reply), &reply_len);
        check("TC-1-10 버퍼보다 큰 입력 그대로", reply_len == len && memcmp(reply, data, len) == 0,
              "응답이 다르다");
        check("TC-1-10 상대 종료로 반환", result == ECHO_PEER_CLOSED, "ECHO_PEER_CLOSED가 아니다");
        free(data);
    }

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
