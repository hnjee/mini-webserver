/*
 * Step 5 단위 테스트(TC-5-11): server_loop를 자식 프로세스에서 AF_UNIX 듣는 소켓으로 돌리고 클라이언트로 시험한다.
 * Step 6(TC-6-13 루프 범위): 127.0.0.1 TCP 백엔드(자식 프로세스)로 라우팅해 두 소켓 등록·백엔드 소켓 교체·504 중 동시 정적·502를 본다.
 * - accept 재시도 오류는 링크 단계 -Wl,--wrap=accept4로 주입한다(제품 코드에는 시험용 분기가 없다).
 * - 끝난 연결 fd 재사용은 결정적으로 재현하지 않는다. 같은 회차에 끝나는 연결과 새 연결을 섞은 부하에서
 *   연결별 응답 정확성과 서버 생존·AddressSanitizer 보고 없음으로 본다.
 * 워커 확장(docs/extensions/worker-pool): epoll_wait·pthread_create도 --wrap으로 주입한다.
 * - epoll_wait 관문: 시험이 루프를 epoll_wait 직전에 세워 두고 두 소켓을 함께 준비시켜 같은 배치를 만든다.
 * - epoll_wait 실패·워커 일부 생성 실패: 루프가 -1로 돌아오고, 자식이 exit()로 끝나 LeakSanitizer 누수 확인까지 한다.
 */
#define _GNU_SOURCE /* accept4 */

#include "../src/server_loop.h"
#include "../src/access_log.h"
#include "../src/http_conn.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static char tmp[256], root[PATH_MAX], sock_path[PATH_MAX], log_path[PATH_MAX], access_path[PATH_MAX]; /* tmp는 mkdtemp의 짧은 경로 */
static const char index_body[] = "<h1>loop</h1>\n";
static route_table *loop_routes; /* NULL이면 -c 생략(Step 5와 같음) */

/* TC-7-13: 첫 응답의 마지막 write가 끝난 직후 워커를 멈춘다. */
static int write_gate_r = -1, write_ack_w = -1;
static int output_gate_r = -1, output_ack_w = -1;
static atomic_int output_gate_once, write_gate_once;
static int progress_w = -1, throttle_output;
ssize_t __real_write(int fd, const void *buf, size_t count);
ssize_t __wrap_write(int fd, const void *buf, size_t count)
{
    ssize_t n;
    if (output_gate_r >= 0 && count == 65536 && !atomic_exchange(&output_gate_once, 1)) {
        char ch;
        /* 아직 write하지 않은 연결 출력 버퍼 64KB가 보장된다. */
        if (__real_write(output_ack_w, "U", 1) != 1 || read(output_gate_r, &ch, 1) != 1 || ch != 'R') _exit(7);
    }
    n = __real_write(fd, buf, count);
    if (progress_w >= 0 && n > 0 && count > 0 && ((const char *)buf)[0] == 'B') {
        if (__real_write(progress_w, "P", 1) != 1) _exit(9);
    }
    if (write_gate_r >= 0 && n == (ssize_t)(sizeof(index_body) - 1) &&
        count == sizeof(index_body) - 1 && memcmp(buf, index_body, count) == 0 &&
        !atomic_exchange(&write_gate_once, 1)) {
        char ch;
        if (__real_write(write_ack_w, "W", 1) != 1 || read(write_gate_r, &ch, 1) != 1 || ch != 'R') _exit(5);
    }
    return n;
}

/* TC-7-13 늦은 신호: 경계 차례 완료를 알린 eventfd를 루프가 읽은 직후(회수 전) 실제 SIGTERM을 낸다. */
static int late_signal_armed, late_evfd = -1;
static atomic_int late_boundary_done, late_injected, late_parsed_after;
/* TC-7-13 admission 마스크 관문: 2 = 마스크 직전 신호, 3 = 마스크 구간 안 신호(보류 → 삽입 뒤 전달). */
static int mask_mode;
static atomic_int mask_inserted, mask_handler_ran;

static void mark(const char *s)
{
    if (__real_write(2, s, strlen(s)) < 0) _exit(11);
}

static void stop_handler(int signo)
{
    if (mask_mode == 3 && atomic_load(&late_injected)) {
        atomic_store(&mask_handler_ran, 1);
        if (!atomic_load(&mask_inserted)) mark("HANDLER_BEFORE_INSERT\n");
    }
    server_loop_request_stop(signo);
}

int __real_pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    int arm = mask_mode && how == SIG_BLOCK && set != NULL && sigismember(set, SIGTERM) == 1 &&
              atomic_load(&late_boundary_done) && !atomic_load(&late_injected);
    int restoring = mask_mode == 3 && how == SIG_SETMASK && atomic_load(&late_injected) &&
                    !atomic_load(&mask_handler_ran);
    int r;
    if (arm && mask_mode == 2) {
        atomic_store(&late_injected, 1);
        mark("PREMASK_SIGNAL\n");
        raise(SIGTERM); /* 막기 전: 핸들러가 바로 돌고 admission은 이를 봐야 한다 */
    }
    r = __real_pthread_sigmask(how, set, old);
    if (arm && mask_mode == 3) {
        atomic_store(&late_injected, 1);
        mark("INMASK_SIGNAL\n");
        raise(SIGTERM); /* 막은 뒤: 보류돼야 한다 */
        if (atomic_load(&mask_handler_ran)) mark("HANDLER_RAN_INSIDE_MASK\n");
    }
    if (restoring && atomic_load(&mask_handler_ran)) mark("PENDING_DELIVERED_ON_RESTORE\n");
    return r;
}

int __real_pthread_cond_signal(pthread_cond_t *cond);
int __wrap_pthread_cond_signal(pthread_cond_t *cond)
{
    if (mask_mode == 3 && atomic_load(&late_injected)) atomic_store(&mask_inserted, 1); /* submit의 큐 삽입 뒤 호출 */
    return __real_pthread_cond_signal(cond);
}
int __real_eventfd(unsigned int initval, int flags);
int __wrap_eventfd(unsigned int initval, int flags)
{
    int fd = __real_eventfd(initval, flags);
    if (late_signal_armed) late_evfd = fd;
    return fd;
}
ssize_t __real_read(int fd, void *buf, size_t count);
ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    ssize_t n = __real_read(fd, buf, count);
    if (late_signal_armed && fd == late_evfd && n > 0 && atomic_load(&late_boundary_done) &&
        !atomic_exchange(&late_injected, 1)) {
        static const char mark[] = "LATE_SIGNAL_INJECTED\n";
        if (__real_write(2, mark, sizeof(mark) - 1) < 0) _exit(11);
        raise(SIGTERM); /* 루프 스레드에서 collect_done 전에 실제 핸들러가 돈다 */
    }
    return n;
}
http_conn_step __real_http_conn_run(http_conn *c, int wake, long long now);
http_conn_step __wrap_http_conn_run(http_conn *c, int wake, long long now)
{
    int boundary = c->at_boundary;
    http_conn_step s;
    if ((late_signal_armed || mask_mode) && boundary && atomic_load(&late_injected) && !atomic_exchange(&late_parsed_after, 1)) {
        static const char mark[] = "LATE_NEXT_REQUEST_STARTED\n";
        if (__real_write(2, mark, sizeof(mark) - 1) < 0) _exit(11);
    }
    s = __real_http_conn_run(c, wake, now);
    if ((late_signal_armed || mask_mode) && c->at_boundary) atomic_store(&late_boundary_done, 1);
    return s;
}

/* ---- accept4 주입: 성공 after_success번 뒤 fail_count번 ECONNABORTED(D-07 재시도 오류) ---- */
static long wrap_after_success = -1; /* -1이면 주입 없음 */
static long wrap_fail_count;
static long wrap_successes;

int __real_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
int __wrap_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);

int __wrap_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    int r;
    if (wrap_after_success >= 0 && wrap_successes >= wrap_after_success && wrap_fail_count > 0) {
        wrap_fail_count--;
        errno = ECONNABORTED;
        return -1;
    }
    r = __real_accept4(fd, addr, len, flags);
    if (r >= 0) {
        if (throttle_output) {
            int size = 8192;
            if (setsockopt(r, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0) _exit(10);
        }
        wrap_successes++;
    }
    return r;
}

/* ---- epoll_wait 주입: fail_at번째 호출을 EIO로 실패시킨다. 관문이 켜져 있으면 'P'를 받은 뒤 'R'까지 멈춘다 ---- */
static long wrap_epoll_fail_at = -1; /* -1이면 주입 없음 */
static long wrap_epoll_calls;
static int gate_r = -1, gate_w = -1, ack_r = -1, ack_w = -1;

int __real_epoll_wait(int epfd, struct epoll_event *events, int max, int timeout);
int __wrap_epoll_wait(int epfd, struct epoll_event *events, int max, int timeout);

int __wrap_epoll_wait(int epfd, struct epoll_event *events, int max, int timeout)
{
    char ch;
    if (++wrap_epoll_calls == wrap_epoll_fail_at) {
        errno = EIO;
        return -1;
    }
    while (gate_r >= 0 && read(gate_r, &ch, 1) == 1) { /* gate_r는 논블로킹 */
        if (ch == 'P') {
            struct pollfd p = {gate_r, POLLIN, 0};
            if (write(ack_w, "p", 1) != 1) {
                _exit(4);
            }
            while (poll(&p, 1, -1) < 0 || read(gate_r, &ch, 1) != 1 || ch != 'R') {
            }
        }
    }
    return __real_epoll_wait(epfd, events, max, timeout);
}

/* ---- pthread_create 주입: fail_at번째 호출을 EAGAIN으로 실패시킨다 ---- */
static long wrap_thread_fail_at = -1;
static long wrap_thread_calls;

int __real_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);

int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    if (++wrap_thread_calls == wrap_thread_fail_at) {
        return EAGAIN;
    }
    return __real_pthread_create(t, a, fn, arg);
}

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

static void build_root(void)
{
    char p[PATH_MAX + 16], made[64];
    int fd;

    setup_or_die(mkdtemp(strcpy(made, "/tmp/test_server_loop-XXXXXX")) != NULL, "mkdtemp");
    setup_or_die(realpath(made, p) != NULL && strlen(p) < sizeof(tmp), "realpath");
    strcpy(tmp, p);
    snprintf(root, sizeof(root), "%s/www", tmp);
    snprintf(sock_path, sizeof(sock_path), "%s/s", tmp);
    snprintf(log_path, sizeof(log_path), "%s/server.log", tmp);
    snprintf(access_path, sizeof(access_path), "%s/access.log", tmp);
    setup_or_die(mkdir(root, 0755) == 0, "mkdir www");
    snprintf(p, sizeof(p), "%s/index.html", root);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    setup_or_die(fd >= 0 && write(fd, index_body, sizeof(index_body) - 1) == (ssize_t)sizeof(index_body) - 1, "index");
    close(fd);
    snprintf(p, sizeof(p), "%s/big.bin", root);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    setup_or_die(fd >= 0, "big.bin");
    {
        char block[65536];
        int i;
        memset(block, 'B', sizeof(block));
        for (i = 0; i < 64; i++) setup_or_die(write(fd, block, sizeof(block)) == sizeof(block), "big.bin write");
    }
    close(fd);
}

static void remove_root(void)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    if (system(cmd) != 0) {
        printf("  (임시 폴더 정리 실패: %s)\n", tmp);
    }
}

/* 듣는 소켓을 만들고 자식에서 루프를 돌린다. 자식 stderr는 log_path로 보낸다. */
static pid_t start_server(void)
{
    struct sockaddr_un addr;
    int fd, log_fd;
    pid_t pid;

    unlink(sock_path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    setup_or_die(fd >= 0, "socket");
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    setup_or_die(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind");
    setup_or_die(listen(fd, SOMAXCONN) == 0, "listen");
    fflush(stdout);
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        char error[256];
        log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd < 0 || dup2(log_fd, 2) < 0) {
            _exit(3);
        }
        close(log_fd);
        alarm(30);
        if (write_gate_r >= 0 || output_gate_r >= 0 || late_signal_armed || mask_mode) {
            struct sigaction sa = {0};
            sa.sa_handler = stop_handler;
            sigemptyset(&sa.sa_mask);
            if (sigaction(SIGTERM, &sa, NULL) != 0) _exit(6);
        }
        if (output_gate_r >= 0) {
            int log = open(access_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log < 0 || dup2(log, STDOUT_FILENO) < 0) _exit(8);
            close(log);
            setvbuf(stdout, NULL, _IOLBF, 0);
            access_log_enable();
        }
        int rc = server_loop_run(fd, root, loop_routes, error, sizeof(error));
        if (rc != 0) fprintf(stderr, "루프 끝: %s\n", error);
        exit(rc == 0 ? 0 : 1); /* _exit가 아니다: LeakSanitizer가 해제 누락을 보고하게 한다 */
    }
    close(fd);
    return pid;
}

/* 자식이 아직 살아 있는가(멈추거나 ASan으로 죽지 않았는가)를 보고 끝낸다. */
static void stop_server(const char *name, pid_t pid)
{
    char label[160], buf[4096];
    int status, alive, fd;
    ssize_t n;

    alive = waitpid(pid, &status, WNOHANG) == 0;
    kill(pid, SIGKILL);
    if (alive) {
        waitpid(pid, &status, 0);
    }
    snprintf(label, sizeof(label), "%s: 서버가 계속 살아 있음", name);
    check(label, alive, "서버 프로세스가 끝났다");
    fd = open(log_path, O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    buf[n > 0 ? n : 0] = '\0';
    if (fd >= 0) {
        close(fd);
    }
    snprintf(label, sizeof(label), "%s: Sanitizer 보고 없음", name);
    check(label, strstr(buf, "Sanitizer") == NULL, "ASan·TSan 보고가 있다");
}

static int dial(void)
{
    struct sockaddr_un addr;
    struct timeval tv = {3, 0};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    setup_or_die(fd >= 0, "socket");
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    setup_or_die(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "connect");
    setup_or_die(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0, "SO_RCVTIMEO");
    return fd;
}

static void write_all(int fd, const char *s)
{
    size_t len = strlen(s);
    while (len > 0) {
        ssize_t n = write(fd, s, len);
        setup_or_die(n > 0, "테스트 write");
        s += n;
        len -= (size_t)n;
    }
}

/* EOF·시간 제한까지 읽는다. */
static size_t read_until_eof(int fd, char *buf, size_t cap)
{
    size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, buf + have, cap - 1 - have);
        if (n <= 0) {
            break;
        }
        have += (size_t)n;
        if (have == cap - 1) {
            break;
        }
    }
    buf[have] = '\0';
    return have;
}

/* 200 응답 count개(본문 index_body)만큼 읽는다. 받은 개수를 돌려준다. */
static int read_ok_responses(int fd, int count)
{
    char buf[8192];
    size_t have = 0;
    int got = 0;

    while (got < count) {
        char *end;
        ssize_t n;
        buf[have] = '\0';
        end = strstr(buf, "\r\n\r\n");
        if (end != NULL && (size_t)(end + 4 - buf) + strlen(index_body) <= have) {
            size_t len = (size_t)(end + 4 - buf) + strlen(index_body);
            if (strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) != 0 || memcmp(end + 4, index_body, strlen(index_body)) != 0) {
                return got;
            }
            memmove(buf, buf + len, have - len);
            have -= len;
            got++;
            continue;
        }
        n = read(fd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0) {
            return got;
        }
        have += (size_t)n;
    }
    return got;
}

/* 로그 파일에서 needle이 나온 횟수(최대 timeout 초까지 기다린다). */
static int log_count_wait(const char *needle, int want, double timeout)
{
    struct timespec gap = {0, 50 * 1000 * 1000};
    int tries = (int)(timeout / 0.05), count = 0;

    while (tries-- > 0) {
        char buf[16384];
        int fd = open(log_path, O_RDONLY);
        ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
        const char *p = buf;
        if (fd >= 0) {
            close(fd);
        }
        buf[n > 0 ? n : 0] = '\0';
        for (count = 0; (p = strstr(p, needle)) != NULL; p += strlen(needle)) {
            count++;
        }
        if (count >= want) {
            return count;
        }
        nanosleep(&gap, NULL);
    }
    return count;
}

/* HUP + 남은 입력: 요청을 쓰고 바로 닫은 연결도 저장된 요청을 판정·응답 시도하고 실제 write 결과(EPIPE)를 기록한다. */
static void test_hup_with_pending_input(void)
{
    pid_t pid = start_server();
    int a = dial(), b = dial(), c;
    char buf[4096];

    write_all(a, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    close(a); /* 서버 쪽에는 HUP과 남은 요청이 함께 보인다 */
    write_all(b, "POST /x HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n");
    close(b);
    check("TC-5-11 HUP + 남은 요청 2건: 처리 후 쓰기 실패 EPIPE가 연결마다 기록됨",
          log_count_wait("연결 오류로 닫음: Broken pipe", 2, 3.0) == 2, "로그가 2줄이 아니다");
    c = dial();
    write_all(c, "GET / HTTP/1.0\r\n\r\n");
    read_until_eof(c, buf, sizeof(buf));
    close(c);
    check("TC-5-11 HUP 연결 뒤에도 다른 연결 응답", strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0, buf);
    stop_server("TC-5-11 HUP", pid);
}

/* 배치 크기(64)와 회차당 accept 상한을 넘는 준비 연결: 모두 순환 처리된다. */
static void test_many_ready(void)
{
    enum { N = 200 };
    static int fds[N];
    pid_t pid = start_server();
    int i, ok = 0;

    for (i = 0; i < N; i++) {
        fds[i] = dial();
    }
    for (i = 0; i < N; i++) {
        write_all(fds[i], "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    }
    for (i = 0; i < N; i++) {
        ok += read_ok_responses(fds[i], 2) == 2;
        close(fds[i]);
    }
    check("TC-5-11 준비 연결 200개(배치 64 초과) 모두 파이프라인 응답 2개씩", ok == N, "응답을 못 받은 연결이 있다");
    stop_server("TC-5-11 배치 초과", pid);
}

/* 끝나는 연결과 새 연결이 섞인 부하: 끝난 연결 해제·fd 재사용 뒤에도 연결별 응답이 정확하다. */
static void test_finish_and_reuse(void)
{
    enum { ROUNDS = 20, K = 30 };
    pid_t pid = start_server();
    int r, i, ok = 0, total = 0;

    for (r = 0; r < ROUNDS; r++) {
        int closing[K], keeping[K];
        for (i = 0; i < K; i++) {
            closing[i] = dial();
            write_all(closing[i], "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
            close(closing[i]); /* 서버 쪽에서 EPIPE·끝으로 정리된다 */
            keeping[i] = dial();
            write_all(keeping[i], "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.0\r\n\r\n");
        }
        for (i = 0; i < K; i++) {
            char tail;
            int got = read_ok_responses(keeping[i], 2);
            ok += got == 2 && read(keeping[i], &tail, 1) == 0; /* 1.0 응답 뒤 서버가 닫음 */
            total++;
            close(keeping[i]);
        }
    }
    check("TC-5-11 끝나는 연결과 섞인 새 연결 600개: 응답 2개 뒤 EOF", ok == total, "응답이 섞이거나 빠졌다");
    stop_server("TC-5-11 끝난 연결 재사용", pid);
}

/* accept 재시도 오류 연속: 무한히 실패해도 이미 받은 연결은 계속 처리된다. 유한하면 결국 새 연결도 받는다. */
static void test_accept_retry_errors(void)
{
    pid_t pid;
    int b, c;
    char buf[4096];

    wrap_after_success = 1; /* 첫 연결만 받고, 그 뒤 accept는 계속 ECONNABORTED */
    wrap_fail_count = LONG_MAX;
    pid = start_server();
    b = dial();
    write_all(b, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    check("TC-5-11 accept 주입 전 연결 B 응답", read_ok_responses(b, 1) == 1, "응답 없음");
    c = dial(); /* 듣는 소켓이 계속 준비 상태가 되고 accept는 매번 실패한다 */
    write_all(b, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    check("TC-5-11 accept 재시도 오류가 끝없이 이어져도 B는 계속 응답받음", read_ok_responses(b, 1) == 1,
          "B가 멈췄다(accept에 묶임)");
    close(b);
    close(c);
    stop_server("TC-5-11 accept 무한 실패", pid);

    wrap_after_success = 0;
    wrap_fail_count = 500;
    pid = start_server();
    c = dial();
    write_all(c, "GET / HTTP/1.0\r\n\r\n");
    read_until_eof(c, buf, sizeof(buf));
    close(c);
    check("TC-5-11 accept 재시도 오류 500번 뒤 새 연결도 받아 응답", strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0, buf);
    stop_server("TC-5-11 accept 유한 실패", pid);
    wrap_after_success = -1;
}

/* ---- Step 6: 루프 수준 프록시 ---- */

/* 127.0.0.1의 빈 포트에 듣는 TCP 소켓. 포트를 돌려준다. */
static int tcp_listen(int *port_out)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;

    setup_or_die(fd >= 0, "socket");
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setup_or_die(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(fd, 64) == 0, "bind/listen");
    setup_or_die(getsockname(fd, (struct sockaddr *)&a, &len) == 0, "getsockname");
    *port_out = ntohs(a.sin_port);
    return fd;
}

/* 요청마다 "<id>:<경로>"로 답하고 닫는 HTTP/1.0 백엔드(자식). 요청 머리 끝까지만 읽는다. */
static pid_t start_backend(char id, int *port)
{
    int fd = tcp_listen(port);
    pid_t pid;

    fflush(stdout);
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        alarm(60);
        for (;;) {
            char req[8192], body[600], resp[800];
            size_t have = 0;
            int c = accept(fd, NULL, NULL), n;
            char *sp, *sp2;
            if (c < 0) {
                continue;
            }
            while (have < sizeof(req) - 1) {
                ssize_t r = read(c, req + have, sizeof(req) - 1 - have);
                if (r <= 0) {
                    break;
                }
                have += (size_t)r;
                req[have] = '\0';
                if (strstr(req, "\r\n\r\n") != NULL) {
                    break;
                }
            }
            req[have] = '\0';
            sp = strchr(req, ' ');
            sp2 = sp != NULL ? strchr(sp + 1, ' ') : NULL;
            n = snprintf(body, sizeof(body), "%c:%.*s", id, sp2 != NULL ? (int)(sp2 - sp - 1) : 0, sp != NULL ? sp + 1 : "");
            n = snprintf(resp, sizeof(resp), "HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n%s", n, body);
            if (write(c, resp, (size_t)n) < 0) {
                /* 시험 백엔드: 쓰기 실패는 무시한다 */
            }
            close(c);
        }
    }
    close(fd);
    return pid;
}

static void set_routes(const char *text)
{
    static route_table table;
    char error[256];
    setup_or_die(route_parse(text, strlen(text), &table, error, sizeof(error)) == 0 &&
                     route_resolve(&table, error, sizeof(error)) == 0,
                 error);
    loop_routes = &table;
}

/* 길이 있는 응답 하나를 읽어 본문을 body에 둔다. 상태 줄 첫 12바이트("HTTP/1.1 200")를 status에 둔다. 실패면 -1. */
static int read_response(int fd, char *status, char *body, size_t cap)
{
    static char buf[16384];
    static size_t have;
    static int last_fd = -1;
    char *end, *cl;
    size_t head, len;

    if (fd != last_fd) {
        have = 0;
        last_fd = fd;
    }
    for (;;) {
        buf[have] = '\0';
        end = strstr(buf, "\r\n\r\n");
        if (end != NULL) {
            cl = strstr(buf, "Content-Length: ");
            head = (size_t)(end + 4 - buf);
            len = cl != NULL && cl < end ? strtoul(cl + 16, NULL, 10) : 0;
            if (have >= head + len) {
                memcpy(status, buf, 12);
                status[12] = '\0';
                snprintf(body, cap, "%.*s", (int)len, buf + head);
                memmove(buf, buf + head + len, have - head - len);
                have -= head + len;
                return 0;
            }
        }
        {
            ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
            if (n <= 0) {
                return -1;
            }
            have += (size_t)n;
        }
    }
}

static double seconds_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) / 1e9;
}

/* 같은 연결의 /api/ 요청이 두 백엔드에 번갈아 가고, 정적 요청과 섞여도 응답이 정확하다(요청마다 새 백엔드 소켓). */
static void test_proxy_round_robin_and_churn(void)
{
    enum { CLIENTS = 30, ROUNDS = 5 };
    int pa, pb, i, r, ok = 0, seq_ok;
    pid_t ba = start_backend('A', &pa), bb = start_backend('B', &pb), pid;
    char text[128], status[16], body[256], seq[64] = "";
    int fds[CLIENTS];

    snprintf(text, sizeof(text), "route /api/ 127.0.0.1:%d 127.0.0.1:%d\n", pa, pb);
    set_routes(text);
    pid = start_server();
    fds[0] = dial();
    for (i = 0; i < 4; i++) {
        write_all(fds[0], "GET /api/x?q=1 HTTP/1.1\r\nHost: x\r\n\r\n");
        if (read_response(fds[0], status, body, sizeof(body)) == 0) {
            strcat(seq, body);
            strcat(seq, " ");
        }
    }
    close(fds[0]);
    seq_ok = strcmp(seq, "A:/x?q=1 B:/x?q=1 A:/x?q=1 B:/x?q=1 ") == 0;
    check("TC-6-01 루프: 한 연결의 /api/ 요청이 두 백엔드에 번갈아 감(접두 /api/ → /)", seq_ok, seq);
    for (i = 0; i < CLIENTS; i++) {
        fds[i] = dial();
    }
    for (r = 0; r < ROUNDS; r++) {
        for (i = 0; i < CLIENTS; i++) {
            write_all(fds[i], "GET /api/k HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
        }
        for (i = 0; i < CLIENTS; i++) {
            char s2[16], b2[256];
            ok += read_response(fds[i], status, body, sizeof(body)) == 0 && strcmp(status, "HTTP/1.1 200") == 0 &&
                  (strcmp(body, "A:/k") == 0 || strcmp(body, "B:/k") == 0) &&
                  read_response(fds[i], s2, b2, sizeof(b2)) == 0 && strcmp(b2, index_body) == 0;
        }
    }
    for (i = 0; i < CLIENTS; i++) {
        close(fds[i]);
    }
    check("TC-6-13 루프: 30연결 × 5회 프록시+정적 파이프라인(백엔드 소켓 150개 교체) 모두 정확", ok == CLIENTS * ROUNDS,
          "응답이 섞이거나 빠졌다");
    stop_server("TC-6-13 프록시 부하", pid);
    kill(ba, SIGKILL);
    kill(bb, SIGKILL);
    waitpid(ba, NULL, 0);
    waitpid(bb, NULL, 0);
}

/* 응답하지 않는 백엔드(accept도 안 함)로 가는 요청은 5초 뒤 504, 그동안 다른 연결의 정적 요청은 바로 응답한다. 닫힌 포트는 502. */
static void test_proxy_504_502_concurrent(void)
{
    int silent_port, closed_port, silent = tcp_listen(&silent_port), closed = tcp_listen(&closed_port);
    char text[160], status[16], body[256];
    struct timespec t0;
    double static_s = -1, timeout_s;
    pid_t pid;
    int a, b;

    close(closed); /* 이 포트는 이제 닫혀 있다(거부) */
    snprintf(text, sizeof(text), "route /slow/ 127.0.0.1:%d\nroute /down/ 127.0.0.1:%d\n", silent_port, closed_port);
    set_routes(text);
    pid = start_server();
    a = dial();
    b = dial();
    {
        struct timeval tv = {8, 0};
        setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    write_all(a, "GET /slow/x HTTP/1.1\r\nHost: x\r\n\r\n");
    write_all(b, "GET /down/x HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    check("TC-6-02 루프: 닫힌 백엔드 → 502", read_response(b, status, body, sizeof(body)) == 0 &&
                                                strcmp(status, "HTTP/1.1 502") == 0,
          status);
    check("TC-6-04 루프: 504 대기 중 다른 연결의 정적 요청 바로 응답",
          read_response(b, status, body, sizeof(body)) == 0 && strcmp(body, index_body) == 0 &&
              (static_s = seconds_since(&t0)) < 1.0,
          body);
    check("TC-6-03 루프: 무응답 백엔드 → 504", read_response(a, status, body, sizeof(body)) == 0 &&
                                                 strcmp(status, "HTTP/1.1 504") == 0,
          status);
    timeout_s = seconds_since(&t0);
    {
        char label[160];
        snprintf(label, sizeof(label), "TC-6-03 루프: 504는 약 5초 뒤(%.2f초, 정적은 %.3f초)", timeout_s, static_s);
        check(label, timeout_s >= 4.9 && timeout_s < 6.5, "시간 범위 밖");
    }
    write_all(a, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    check("TC-6-11 루프: 504 뒤 같은 연결 유지, 다음 정적 요청 200",
          read_response(a, status, body, sizeof(body)) == 0 && strcmp(body, index_body) == 0, body);
    close(a);
    close(b);
    close(silent);
    stop_server("TC-6-13 504·502", pid);
}

/* ---- 워커 확장(docs/extensions/worker-pool) ---- */

/* 주입으로 끝나야 하는 서버: timeout 초 안에 끝나는지 보고 로그를 log에 담는다. 끝나지 않으면 죽이고 -1. */
static int wait_exit(pid_t pid, double timeout, char *log, size_t cap)
{
    struct timespec gap = {0, 20 * 1000 * 1000};
    int status = -1, tries = (int)(timeout / 0.02), fd;
    ssize_t n;

    while (tries-- > 0 && waitpid(pid, &status, WNOHANG) == 0) {
        nanosleep(&gap, NULL);
    }
    if (tries < 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        status = -1;
    }
    fd = open(log_path, O_RDONLY);
    n = fd >= 0 ? read(fd, log, cap - 1) : -1;
    log[n > 0 ? n : 0] = '\0';
    if (fd >= 0) {
        close(fd);
    }
    return status;
}

/* 업로드 중 같은 배치에 클라이언트 본문·백엔드 응답 머리 일부가 함께 오고 마감이 이미 지났다.
 * 알림마다 한 차례씩이면 클라이언트 본문 read가 진행으로 마감을 다시 잡아 연결이 살아남는다.
 * 두 원인을 한 차례로 합치면 백엔드 read가 예산을 써 본문 read가 빠지고, 같은 회차 마감 확인에서 닫힌다. */
static void test_same_batch_client_and_backend(void)
{
    int port, bl = tcp_listen(&port), pg[2], pa[2], a, be, flags;
    char text[96], req[8192], status[16], body[64], ch;
    size_t have = 0, body_seen = 0;
    pid_t pid;
    struct timeval tv = {12, 0};

    snprintf(text, sizeof(text), "route /up/ 127.0.0.1:%d\n", port);
    set_routes(text);
    setup_or_die(pipe(pg) == 0 && pipe(pa) == 0, "pipe");
    flags = fcntl(pg[0], F_GETFL);
    setup_or_die(flags >= 0 && fcntl(pg[0], F_SETFL, flags | O_NONBLOCK) == 0, "O_NONBLOCK");
    gate_r = pg[0];
    ack_w = pa[1];
    pid = start_server();
    close(pg[0]);
    close(pa[1]);
    gate_r = ack_w = -1;
    gate_w = pg[1];
    ack_r = pa[0];

    a = dial();
    setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    write_all(a, "POST /up/x HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nAB");
    be = accept(bl, NULL, NULL);
    setup_or_die(be >= 0, "accept backend");
    setsockopt(be, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (have < sizeof(req) - 1) { /* 머리와 본문 AB까지 받는다 */
        ssize_t r = read(be, req + have, sizeof(req) - 1 - have);
        char *end;
        setup_or_die(r > 0, "backend read");
        have += (size_t)r;
        req[have] = '\0';
        end = strstr(req, "\r\n\r\n");
        if (end != NULL && have - (size_t)(end + 4 - req) >= 2) {
            body_seen = have - (size_t)(end + 4 - req);
            break;
        }
    }
    {
        struct timespec settle = {0, 300 * 1000 * 1000};
        int wake;
        nanosleep(&settle, NULL); /* 서버가 본문 대기(클라이언트·백엔드 읽기 등록)로 들어갈 시간 */
        setup_or_die(write(gate_w, "P", 1) == 1, "gate P");
        wake = dial(); /* 루프를 epoll_wait 밖으로 한 번 꺼내 관문에 세운다 */
        setup_or_die(read(ack_r, &ch, 1) == 1, "gate ack");
        sleep(6); /* 업로드 대기 마감(5초)이 지나도록 */
        write_all(a, "CDE");
        write_all(be, "HTTP/1.0 200 OK\r\n");
        setup_or_die(write(gate_w, "R", 1) == 1, "gate R");
        write_all(a, "FGHIJ");
        while (body_seen < 10) {
            ssize_t r = read(be, req, sizeof(req));
            if (r <= 0) {
                break;
            }
            body_seen += (size_t)r;
        }
        write_all(be, "Content-Length: 4\r\n\r\nOKAY");
        close(be);
        check("워커 확장 같은 배치 클라이언트+백엔드(마감 지남): 본문 끝까지 업로드되고 200 OKAY",
              body_seen == 10 && read_response(a, status, body, sizeof(body)) == 0 &&
                  strcmp(status, "HTTP/1.1 200") == 0 && strcmp(body, "OKAY") == 0,
              body_seen == 10 ? "응답이 없거나 다르다(마감으로 닫힘)" : "백엔드가 본문을 다 받지 못했다");
        close(wake);
    }
    close(a);
    close(bl);
    close(gate_w);
    close(ack_r);
    gate_w = ack_r = -1;
    stop_server("워커 확장 같은 배치", pid);
    loop_routes = NULL;
}

/* 깨우기 손실 없음: 한 연결의 짧은 요청 1000개가 각각 1초 안에 끝난다(손실이면 다른 깨움 없이 멈춘다). */
static void test_sequential_wakeups(void)
{
    enum { N = 1000 };
    pid_t pid = start_server();
    int fd = dial(), i, ok = 0;
    double worst = 0;
    char label[160];

    for (i = 0; i < N; i++) {
        struct timespec t0;
        double s;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        write_all(fd, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        if (read_ok_responses(fd, 1) != 1) {
            break;
        }
        s = seconds_since(&t0);
        worst = s > worst ? s : worst;
        ok++;
    }
    close(fd);
    snprintf(label, sizeof(label), "워커 확장 한 연결 짧은 요청 %d개 모두 1초 안(최대 %.4f초)", N, worst);
    check(label, ok == N && worst < 1.0, "응답이 늦거나 멈췄다");
    stop_server("워커 확장 깨우기", pid);
}

/* 부하 중 epoll_wait 실패 주입: 루프가 워커를 멈추고 모두 해제한 뒤 -1로 돌아온다(교착·누수·ASan 없음). */
static void test_fatal_epoll_wait(void)
{
    enum { CLIENTS = 40 };
    int fds[CLIENTS], i, status;
    char log[4096];
    pid_t pid;

    wrap_epoll_fail_at = 10;
    pid = start_server();
    for (i = 0; i < CLIENTS; i++) {
        fds[i] = dial();
        write_all(fds[i], "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    }
    status = wait_exit(pid, 5.0, log, sizeof(log));
    for (i = 0; i < CLIENTS; i++) {
        close(fds[i]);
    }
    wrap_epoll_fail_at = -1;
    check("워커 확장 부하 중 epoll_wait 실패 → 5초 안에 exit 1(교착 없음)", status >= 0 && WIFEXITED(status) &&
                                                                         WEXITSTATUS(status) == 1,
          "끝나지 않았거나 다른 종료");
    check("워커 확장 epoll_wait 실패 이유가 로그에 있음", strstr(log, "epoll_wait 실패") != NULL, log);
    check("워커 확장 epoll_wait 실패 뒤 Sanitizer(누수 포함) 보고 없음", strstr(log, "Sanitizer") == NULL, log);
}

/* 시작 때 워커 일부만 만들어짐(3번째 pthread_create 실패): 만든 워커만 join하고 -1로 돌아온다. */
static void test_partial_workers(void)
{
    int status;
    char log[4096];
    pid_t pid;

    wrap_thread_fail_at = 3;
    pid = start_server();
    status = wait_exit(pid, 5.0, log, sizeof(log));
    wrap_thread_fail_at = -1;
    check("워커 확장 워커 3번째 생성 실패 → 5초 안에 exit 1", status >= 0 && WIFEXITED(status) &&
                                                               WEXITSTATUS(status) == 1,
          "끝나지 않았거나 다른 종료");
    check("워커 확장 워커 생성 실패 이유가 로그에 있음", strstr(log, "워커 스레드 만들기 실패") != NULL, log);
    check("워커 확장 워커 생성 실패 뒤 Sanitizer(누수 포함) 보고 없음", strstr(log, "Sanitizer") == NULL, log);
}

/* 정지 신호를 첫 응답의 마지막 write 완료 직후에 넣고, 둘째 요청은 시작하지 않는다. */
static void test_shutdown_request_boundary(void)
{
    int pg[2], pa[2], fd, status, refused = 0, i;
    char ch, reply[4096], log[4096];
    struct sockaddr_un addr = {0};
    struct timespec gap = {0, 10 * 1000 * 1000};
    pid_t pid;
    setup_or_die(pipe(pg) == 0 && pipe(pa) == 0, "boundary pipes");
    write_gate_r = pg[0];
    write_ack_w = pa[1];
    write_gate_once = 0;
    pid = start_server();
    close(pg[0]);
    close(pa[1]);
    write_gate_r = write_ack_w = -1;
    fd = dial();
    write_all(fd, "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    setup_or_die(read(pa[0], &ch, 1) == 1 && ch == 'W', "첫 응답 마지막 write 관문");
    setup_or_die(kill(pid, SIGTERM) == 0, "boundary signal");
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    for (i = 0; i < 100; i++) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        setup_or_die(s >= 0, "boundary socket");
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno == ECONNREFUSED) refused = 1;
        close(s);
        if (refused) break;
        nanosleep(&gap, NULL);
    }
    check("TC-7-13 정지 관문: 워커 회수 전에 accept 중단", refused, "듣는 소켓이 아직 열려 있다");
    setup_or_die(write(pg[1], "R", 1) == 1, "boundary release");
    read_until_eof(fd, reply, sizeof(reply));
    close(fd);
    status = wait_exit(pid, 3.0, log, sizeof(log));
    check("TC-7-13 첫 요청만 응답·버퍼 속 둘째 요청은 폐기",
          strstr(reply, "HTTP/1.1 200") != NULL && strstr(reply + 1, "HTTP/1.1 200") == NULL, reply);
    check("TC-7-13 바쁜 워커 회수·join·exit 0", status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
          strstr(log, "Sanitizer") == NULL, log);
    close(pg[1]);
    close(pa[0]);
}

/* F1 회귀: 정지 신호가 epoll_wait 뒤 정지 확인을 지나 완료 eventfd 읽기 직후에 와도 둘째 요청을 시작하지 않는다. */
static void test_late_signal_before_collect(void)
{
    int fd, status, responses = 0;
    char reply[4096], log[4096];
    const char *p;
    pid_t pid;
    late_signal_armed = 1;
    pid = start_server();
    late_signal_armed = 0;
    fd = dial();
    write_all(fd, "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /second HTTP/1.1\r\nHost: x\r\n\r\n");
    read_until_eof(fd, reply, sizeof(reply));
    close(fd);
    status = wait_exit(pid, 3.0, log, sizeof(log));
    for (p = reply; (p = strstr(p, "HTTP/1.1 ")) != NULL; p++) responses++;
    check("TC-7-13 늦은 신호 관문 실제 주입", strstr(log, "LATE_SIGNAL_INJECTED") != NULL, log);
    check("TC-7-13 늦은 신호 뒤 경계 연결 재실행 없음", strstr(log, "LATE_NEXT_REQUEST_STARTED") == NULL, log);
    check("TC-7-13 늦은 신호(완료 eventfd 읽기 직후): 첫 응답만, 둘째 요청 무응답",
          responses == 1 && strstr(reply, "HTTP/1.1 200") != NULL, reply);
    check("TC-7-13 늦은 신호: exit 0·Sanitizer 없음", status >= 0 && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0 && strstr(log, "Sanitizer") == NULL, log);
}

/*
 * F1 잔여: 다음 요청 admission(큐 삽입)은 두 신호를 막은 구간에서 최신 정지를 확인한 뒤 일어난다.
 * mode 2: 막기 직전에 처리된 신호 → 넣지 않는다(첫 응답만). mode 3: 막은 구간의 신호 → 보류, 삽입 뒤 복원에서 전달(둘째 요청은 이미 받은 요청).
 */
static void test_admission_mask(int mode)
{
    int fd, status, responses = 0;
    char reply[4096], log[4096], label[160];
    const char *p, *name = mode == 2 ? "마스크 직전 신호" : "마스크 구간 신호";
    pid_t pid;
    mask_mode = mode;
    pid = start_server();
    mask_mode = 0;
    fd = dial();
    write_all(fd, "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /second HTTP/1.1\r\nHost: x\r\n\r\n");
    read_until_eof(fd, reply, sizeof(reply));
    close(fd);
    status = wait_exit(pid, 3.0, log, sizeof(log));
    for (p = reply; (p = strstr(p, "HTTP/1.1 ")) != NULL; p++) responses++;
    snprintf(label, sizeof(label), "TC-7-13 %s 관문 실제 주입", name);
    check(label, strstr(log, mode == 2 ? "PREMASK_SIGNAL" : "INMASK_SIGNAL") != NULL, log);
    if (mode == 2) {
        check("TC-7-13 마스크 직전 신호: 둘째 요청 admission 없음·첫 응답만",
              strstr(log, "LATE_NEXT_REQUEST_STARTED") == NULL && responses == 1, reply);
    } else {
        check("TC-7-13 마스크 구간 신호: 구간 안에서는 핸들러 보류",
              strstr(log, "HANDLER_RAN_INSIDE_MASK") == NULL, log);
        check("TC-7-13 마스크 구간 신호: 큐 삽입 뒤 마스크 복원에서 전달",
              strstr(log, "HANDLER_BEFORE_INSERT") == NULL && strstr(log, "PENDING_DELIVERED_ON_RESTORE") != NULL, log);
        check("TC-7-13 마스크 구간 신호: 이미 받은 둘째 요청은 응답 뒤 닫음",
              strstr(log, "LATE_NEXT_REQUEST_STARTED") != NULL && responses == 2, reply);
    }
    snprintf(label, sizeof(label), "TC-7-13 %s: exit 0·Sanitizer 없음", name);
    check(label, status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && strstr(log, "Sanitizer") == NULL, log);
}

/* 출력 버퍼 64KB를 채운 뒤 write 직전에 멈춰 미송신 관문을 만든다(TC-7-01/05/06). */
static void test_unsent_output_gate(int shutdown)
{
    int pg[2], pa[2], fd, status, refused = 0, i, log_fd;
    char ch, log[4096], access[4096], buf[65536];
    size_t bytes = 0, head = 0;
    struct sockaddr_un addr = {0};
    struct timespec gap = {0, 10 * 1000 * 1000};
    pid_t pid;
    setup_or_die(pipe(pg) == 0 && pipe(pa) == 0, "unsent pipes");
    output_gate_r = pg[0]; output_ack_w = pa[1]; output_gate_once = 0;
    pid = start_server();
    close(pg[0]); close(pa[1]);
    output_gate_r = output_ack_w = -1;
    fd = dial();
    write_all(fd, "GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n");
    setup_or_die(read(pa[0], &ch, 1) == 1 && ch == 'U', "unsent 64KB gate");
    check(shutdown ? "TC-7-05/06 서버 측 미송신 출력 관문" : "TC-7-01 서버 측 미송신 출력 관문",
          ch == 'U', "64KB write 진입 관문 부재");
    if (shutdown) {
        setup_or_die(kill(pid, SIGTERM) == 0, "unsent shutdown signal");
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
        for (i = 0; i < 100; i++) {
            int s = socket(AF_UNIX, SOCK_STREAM, 0);
            setup_or_die(s >= 0, "unsent refuse socket");
            if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno == ECONNREFUSED) refused = 1;
            close(s);
            if (refused) break;
            nanosleep(&gap, NULL);
        }
        check("TC-7-06 정리 중 새 연결 거부", refused, "accept 여전히 열림");
    } else {
        close(fd); /* write 전에 출력이 남았다는 사실을 확인한 뒤 끊는다 */
    }
    setup_or_die(write(pg[1], "R", 1) == 1, "unsent release");
    if (shutdown) {
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (bytes == 0) {
                char *end = memmem(buf, (size_t)n, "\r\n\r\n", 4);
                if (end) head = (size_t)(end + 4 - buf);
            }
            bytes += (size_t)n;
        }
        close(fd);
        status = wait_exit(pid, 6.0, log, sizeof(log));
        check("TC-7-05 진행 중 다운로드 전체 마무리·exit 0",
              head > 0 && bytes == head + 4194304 && status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "응답 길이 또는 정상 종료 실패");
    } else {
        int c, alive;
        nanosleep(&gap, NULL);
        c = dial();
        write_all(c, "GET / HTTP/1.0\r\n\r\n");
        read_until_eof(c, buf, sizeof(buf)); close(c);
        alive = waitpid(pid, &status, WNOHANG) == 0;
        check("TC-7-01 끊긴 다운로드 뒤 서버 유지·200", alive && strncmp(buf, "HTTP/1.1 200", 12) == 0, buf);
        kill(pid, SIGTERM);
        status = wait_exit(pid, 3.0, log, sizeof(log));
        check("TC-7-01 뒤 정상 종료", status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, log);
    }
    log_fd = open(access_path, O_RDONLY);
    i = log_fd >= 0 ? read(log_fd, access, sizeof(access) - 1) : -1;
    access[i > 0 ? i : 0] = '\0';
    if (log_fd >= 0) close(log_fd);
    if (shutdown) {
        check("TC-7-05 완료 요청 한 줄 ok", strstr(access, " 200 ") != NULL &&
              strstr(access, " ok\n") != NULL && strstr(access, " aborted\n") == NULL, access);
    } else {
        check("TC-7-01 끊긴 요청 한 줄 aborted", strstr(access, " 200 ") != NULL &&
              strstr(access, " aborted\n") != NULL && strstr(strstr(access, " aborted\n") + 1, " aborted\n") == NULL,
              access);
    }
    close(pg[1]); close(pa[0]);
}

/* 10초 강제 마감: 소켓에서 실제 성공한 서버 write의 시각을 관측한다. */
static void test_forced_shutdown_progress(void)
{
    int pg[2], pa[2], progress[2], fd, status, i, flags, refused = 0;
    char ch, buf[2048], log[4096], access[4096];
    int seen5 = 0, seen8 = 0, writes = 0, total = 0;
    double exit_elapsed = -1;
    siginfo_t exit_info = {0};
    struct sockaddr_un addr = {0};
    struct timespec gap = {0, 100 * 1000 * 1000}, start;
    pid_t pid;
    setup_or_die(pipe(pg) == 0 && pipe(pa) == 0 && pipe(progress) == 0, "forced pipes");
    output_gate_r = pg[0]; output_ack_w = pa[1]; progress_w = progress[1];
    output_gate_once = 0; throttle_output = 1;
    pid = start_server();
    close(pg[0]); close(pa[1]); close(progress[1]);
    output_gate_r = output_ack_w = progress_w = -1; throttle_output = 0;
    flags = fcntl(progress[0], F_GETFL);
    setup_or_die(flags >= 0 && fcntl(progress[0], F_SETFL, flags | O_NONBLOCK) == 0, "progress nonblock");
    fd = dial();
    {
        int size = 8192;
        setup_or_die(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0, "client rcvbuf");
    }
    write_all(fd, "GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n");
    setup_or_die(read(pa[0], &ch, 1) == 1 && ch == 'U', "forced unsent gate");
    setup_or_die(kill(pid, SIGTERM) == 0, "forced signal");
    clock_gettime(CLOCK_MONOTONIC, &start);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    for (i = 0; i < 100; i++) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        setup_or_die(s >= 0, "forced refuse socket");
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno == ECONNREFUSED) refused = 1;
        close(s);
        if (refused) break;
        nanosleep(&gap, NULL);
    }
    check("TC-7-10 정리 시작 전 미송신 64KB 관문·accept 중단", refused, "정리 미진입");
    setup_or_die(write(pg[1], "R", 1) == 1, "forced release");
    for (i = 0; i < 120; i++) {
        ssize_t n;
        double elapsed = seconds_since(&start);
        if (elapsed >= 11.8) break;
        if (poll(&(struct pollfd){fd, POLLIN, 0}, 1, 0) > 0) {
            n = read(fd, buf, 1024);
            if (n > 0) total += (int)n;
        }
        while ((n = read(progress[0], buf, sizeof(buf))) > 0) {
            writes += (int)n;
            if (elapsed > 5.0) seen5 = 1;
            if (elapsed > 8.0) seen8 = 1;
        }
        setup_or_die(waitid(P_PID, (id_t)pid, &exit_info, WEXITED | WNOWAIT | WNOHANG) == 0, "waitid");
        if (exit_info.si_pid == pid) {
            exit_elapsed = seconds_since(&start);
            break;
        }
        nanosleep(&gap, NULL);
    }
    status = wait_exit(pid, 2.0, log, sizeof(log));
    {
        int a = open(access_path, O_RDONLY);
        ssize_t n = a >= 0 ? read(a, access, sizeof(access) - 1) : -1;
        access[n > 0 ? n : 0] = '\0';
        if (a >= 0) close(a);
    }
    printf("  TC-7-10 계측: 종료 %.2fs, 성공 write %d회, 클라이언트 수신 %dB, 5초 뒤 %d / 8초 뒤 %d\n",
           exit_elapsed, writes, total, seen5, seen8);
    check("TC-7-10 5초 마감보다 늦게 서버 write 성공 계측", writes > 0 && seen5 && seen8,
          "5초·8초 뒤 실제 write 진행 증거 없음");
    {
        char *aborted = strstr(access, " aborted\n");
        check("TC-7-10 10초 상한 강제 종료·응답 미완료", exit_elapsed >= 9.7 && exit_elapsed <= 11.0 &&
              status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && total < 4194304 &&
              strstr(log, "강제 종료 연결 1개") && aborted != NULL &&
              strstr(aborted + 1, " aborted\n") == NULL,
              "종료 시간/로그/응답 불일치");
    }
    close(fd); close(pg[1]); close(pa[0]); close(progress[0]);
}

int main(void)
{
    alarm(120);
    signal(SIGPIPE, SIG_IGN); /* D-05: 서버와 같은 조건 */
    printf("test_server_loop (TC-5-11, TC-6-13 루프 범위, 워커 확장)\n");
    build_root();
    test_hup_with_pending_input();
    test_many_ready();
    test_finish_and_reuse();
    test_accept_retry_errors();
    test_proxy_round_robin_and_churn();
    test_proxy_504_502_concurrent();
    loop_routes = NULL;
    test_same_batch_client_and_backend();
    test_sequential_wakeups();
    test_fatal_epoll_wait();
    test_partial_workers();
    test_shutdown_request_boundary();
    test_late_signal_before_collect();
    test_admission_mask(2);
    test_admission_mask(3);
    test_unsent_output_gate(0);
    test_unsent_output_gate(1);
    test_forced_shutdown_progress();
    remove_root();

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
