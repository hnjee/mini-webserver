/*
 * Step 1 단위 테스트: docs/tests/step1.md 3장 UT-01 ~ UT-12
 * server_listen()은 포트 0으로 열어서 8080 서버와 부딪히지 않게 한다.
 * serve_client()는 socketpair()로 만든 소켓 두 개 중 하나를 넘겨서 확인한다.
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

#define BIG_SIZE (1000 * 1000)

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
    CHECK("UT-01 fd >= 3", fd >= 3);
    if (fd < 0)
        return;

    CHECK("UT-02 LISTEN 상태 (SO_ACCEPTCONN == 1)", get_int_opt(fd, SOL_SOCKET, SO_ACCEPTCONN) == 1);
    CHECK("UT-03 SO_REUSEADDR == 1", get_int_opt(fd, SOL_SOCKET, SO_REUSEADDR) == 1);

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    int ok = getsockname(fd, (struct sockaddr *)&addr, &len) == 0;
    CHECK("UT-04 주소가 0.0.0.0", ok && addr.sin_addr.s_addr == htonl(INADDR_ANY));

    int port = ntohs(addr.sin_port);
    int before = count_open_fds();
    int again = server_listen(port);
    int after = count_open_fds();
    CHECK("UT-05 같은 포트 다시 열면 -1", again == -1);
    CHECK("UT-05 실패해도 fd가 늘지 않음", before == after);
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
        printf("  skip  UT-06 (root이거나 ip_unprivileged_port_start=%d)\n", start);
        return;
    }
    int fd = server_listen(80);
    CHECK("UT-06 80번 포트는 -1", fd == -1);
    if (fd >= 0)
        close(fd);
}

/* 데이터를 보내고 쓰기를 닫은 뒤 serve_client를 돌리고, 돌아온 바이트를 out에 담는다 */
static int echo_once(const char *data, size_t len, char *out, size_t cap, ssize_t *got, int *client_fd_open)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -2;

    if (len > 0)
        write_full(sv[0], data, len);
    shutdown(sv[0], SHUT_WR);

    int r = serve_client(sv[1]);
    *client_fd_open = fcntl(sv[1], F_GETFD) != -1;
    close(sv[1]);

    *got = read_until_eof(sv[0], out, cap);
    close(sv[0]);
    return r;
}

/* UT-07, UT-12: "hi"를 그대로 돌려주고 소켓은 닫지 않음 */
static void ut07_ut12_hi(void)
{
    char out[16];
    ssize_t got = 0;
    int open_after = 0;
    int r = echo_once("hi", 2, out, sizeof(out), &got, &open_after);
    CHECK("UT-07 리턴 0", r == 0);
    CHECK("UT-07 \"hi\"가 돌아옴", got == 2 && memcmp(out, "hi", 2) == 0);
    CHECK("UT-12 serve_client는 소켓을 닫지 않음", open_after);
}

/* UT-08: 널 문자가 섞인 바이트도 그대로 */
static void ut08_binary(void)
{
    const char data[] = {'a', ' ', 'b', '\n', 'c', '\0', 'd'};
    char out[16];
    ssize_t got = 0;
    int open_after = 0;
    int r = echo_once(data, sizeof(data), out, sizeof(out), &got, &open_after);
    CHECK("UT-08 리턴 0", r == 0);
    CHECK("UT-08 7바이트가 그대로 돌아옴", got == (ssize_t)sizeof(data) && memcmp(out, data, sizeof(data)) == 0);
}

/* UT-09: 아무것도 안 보내고 끊기 */
static void ut09_empty(void)
{
    char out[16];
    ssize_t got = -1;
    int open_after = 0;
    int r = echo_once(NULL, 0, out, sizeof(out), &got, &open_after);
    CHECK("UT-09 리턴 0", r == 0);
    CHECK("UT-09 돌아온 바이트 0", got == 0);
}

static char pattern_byte(size_t i)
{
    return (char)((i * 31 + 7) % 251);
}

/* UT-10: 1MB. 자식 1은 serve_client, 자식 2는 쓰기, 부모는 읽기 */
static void ut10_big(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        CHECK("UT-10 socketpair", 0);
        return;
    }

    pid_t server = fork();
    if (server == 0) {
        close(sv[0]);
        int r = serve_client(sv[1]);
        _exit(r == 0 ? 0 : 1);
    }
    close(sv[1]);

    pid_t writer = fork();
    if (writer == 0) {
        char *data = malloc(BIG_SIZE);
        if (data == NULL)
            _exit(1);
        for (size_t i = 0; i < BIG_SIZE; i++)
            data[i] = pattern_byte(i);
        int r = write_full(sv[0], data, BIG_SIZE);
        shutdown(sv[0], SHUT_WR);
        free(data);
        _exit(r == 0 ? 0 : 1);
    }

    char *out = malloc(BIG_SIZE + 1);
    ssize_t got = out != NULL ? read_until_eof(sv[0], out, BIG_SIZE + 1) : -1;
    close(sv[0]);

    int same = got == BIG_SIZE;
    for (size_t i = 0; same && i < BIG_SIZE; i++)
        same = out[i] == pattern_byte(i);
    free(out);

    int st_server = -1, st_writer = -1;
    waitpid(server, &st_server, 0);
    waitpid(writer, &st_writer, 0);

    CHECK("UT-10 1MB가 그대로 돌아옴", same);
    CHECK("UT-10 serve_client 리턴 0", WIFEXITED(st_server) && WEXITSTATUS(st_server) == 0);
}

/* UT-11: 상대가 끊은 뒤 쓰면 -1 (EPIPE), 테스트 프로그램은 죽지 않음 */
static void ut11_peer_closed(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        CHECK("UT-11 socketpair", 0);
        return;
    }
    write_full(sv[0], "hi", 2);
    close(sv[0]);

    int r = serve_client(sv[1]);
    close(sv[1]);
    CHECK("UT-11 리턴 -1", r == -1);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);   /* UT-11에서 테스트 프로그램이 죽지 않게 */
    alarm(20);                  /* serve_client가 멈추면 테스트를 끝냄 */

    printf("test_server\n");
    ut01_to_ut05_listen();
    ut06_low_port();
    ut07_ut12_hi();
    ut08_binary();
    ut09_empty();
    ut10_big();
    ut11_peer_closed();
    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
