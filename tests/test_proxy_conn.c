/*
 * Step 6 단위 테스트: http_conn의 프록시 상태 전이(TC-6-09~TC-6-13 중 연결 하나 범위).
 * - 백엔드는 링크 단계 --wrap=socket·connect·getsockopt로 socketpair 한쪽을 돌려받아 시험이 직접 연기한다
 *   (제품 코드에는 시험용 분기가 없다). connect 결과(즉시 성공·EINPROGRESS·즉시 실패)와 SO_ERROR도 여기서 정한다.
 * - sess_* 구동 함수는 루프와 같은 규칙(현재 대기 소켓만 poll, 업로드 중 백엔드 읽기 추가, RUNNABLE은 내부 실행)으로 돌린다.
 * - 시간 제한 시험은 now를 시험이 정해 넘기는 직접 호출(가짜 시계)로 한다.
 */
#define _GNU_SOURCE

#include "../src/http_conn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SEC 1000000000LL

static int failures;
static char tmp[PATH_MAX], root[PATH_MAX + 8];
static route_table routes;
static const char index_body[] = "<h1>index</h1>\n";

/* ---- 백엔드 주입 ---- */
enum { CONNECT_OK, CONNECT_INPROGRESS, CONNECT_REFUSED };
static int connect_mode = CONNECT_OK;
static int so_error = 0;   /* EINPROGRESS 뒤 SO_ERROR 값 */
static int socket_fail;    /* socket()이 EMFILE로 실패 */
static int peer = -1;      /* 시험이 연기하는 백엔드 쪽 끝 */
static int peer_gen;       /* 새 백엔드 소켓마다 1 늘어난다 */
static int replaced_peer_eof; /* 교체된 이전 백엔드가 프록시에 의해 이미 닫혀 있었는가 */
static int connect_calls;

int __real_socket(int domain, int type, int protocol);
int __wrap_socket(int domain, int type, int protocol);
int __wrap_connect(int fd, const void *addr, socklen_t len);
int __real_getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int __wrap_getsockopt(int fd, int level, int name, void *val, socklen_t *len);

int __wrap_socket(int domain, int type, int protocol)
{
    int sv[2];
    (void)domain;
    (void)protocol;
    if (socket_fail) {
        errno = EMFILE;
        return -1;
    }
    if (socketpair(AF_UNIX, type, 0, sv) != 0) {
        return -1;
    }
    if (peer >= 0) { /* 이전 백엔드: 남은 바이트를 비우고 EOF였는지 기록한 뒤 닫는다 */
        char buf[65536];
        ssize_t n;
        int flags = fcntl(peer, F_GETFL);
        fcntl(peer, F_SETFL, flags | O_NONBLOCK);
        while ((n = read(peer, buf, sizeof(buf))) > 0) {
        }
        replaced_peer_eof = n == 0;
        close(peer);
    }
    peer = sv[1];
    peer_gen++;
    return sv[0];
}

int __wrap_connect(int fd, const void *addr, socklen_t len)
{
    (void)fd;
    (void)addr;
    (void)len;
    connect_calls++;
    if (connect_mode == CONNECT_OK) {
        return 0;
    }
    errno = connect_mode == CONNECT_INPROGRESS ? EINPROGRESS : ECONNREFUSED;
    return -1;
}

int __wrap_getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    if (level == SOL_SOCKET && name == SO_ERROR) {
        *(int *)val = so_error;
        *len = sizeof(int);
        return 0;
    }
    return __real_getsockopt(fd, level, name, val, len);
}

/* ---- 공통 ---- */

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

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * SEC + t.tv_nsec;
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    setup_or_die(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "O_NONBLOCK");
}

static int lowest_free_fd(void)
{
    int fd = open("/dev/null", O_RDONLY);
    setup_or_die(fd >= 0, "open /dev/null");
    close(fd);
    return fd;
}

static void build_root(void)
{
    char p[PATH_MAX + 32];
    int fd;

    setup_or_die(mkdtemp(strcpy(p, "/tmp/test_proxy_conn-XXXXXX")) != NULL, "mkdtemp");
    setup_or_die(realpath(p, tmp) != NULL, "realpath");
    snprintf(root, sizeof(root), "%s/www", tmp);
    setup_or_die(mkdir(root, 0755) == 0, "mkdir");
    snprintf(p, sizeof(p), "%s/index.html", root);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    setup_or_die(fd >= 0 && write(fd, index_body, sizeof(index_body) - 1) == (ssize_t)sizeof(index_body) - 1, "index");
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

static void use_routes(const char *text)
{
    char error[256];
    setup_or_die(route_parse(text, strlen(text), &routes, error, sizeof(error)) == 0, error);
}

/* ---- 루프 흉내 구동 ---- */

#define REPLY_MAX (2 * 1024 * 1024)

typedef struct {
    http_conn *c;
    int client;            /* 시험 쪽 클라이언트 끝(논블로킹) */
    char *send;            /* 클라이언트가 보낼 바이트 */
    size_t send_len, sent;
    int shut_after_send;   /* 다 보내면 쓰기 방향을 닫는다 */
    int shut_done;
    char *reply;           /* 클라이언트가 받은 바이트 */
    size_t reply_len;
    int client_eof;
    char be_got[65536];    /* 백엔드가 받은 바이트(앞부분) */
    size_t be_got_len, be_got_total;
    int peer_reading;      /* 0이면 백엔드는 읽지 않는다(쓰기 막힘 연출) */
    int peer_eof;
    int peer_seen; /* be_got이 가리키는 백엔드 세대 */
    http_conn_step step;
    int finished;
} sess;

static void sess_open(sess *s, const char *data, size_t len, int shut)
{
    int fds[2];

    memset(s, 0, sizeof(*s));
    s->c = malloc(sizeof(*s->c));
    s->reply = malloc(REPLY_MAX);
    s->send = malloc(len + 1);
    setup_or_die(s->c != NULL && s->reply != NULL && s->send != NULL, "malloc");
    memcpy(s->send, data, len);
    s->send_len = len;
    s->shut_after_send = shut;
    s->peer_reading = 1;
    if (peer >= 0) {
        close(peer);
    }
    peer = -1;
    s->peer_seen = peer_gen;
    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);
    s->client = fds[0];
    http_conn_init(s->c, fds[1], root, &routes, NULL, now_ns());
    s->step = HTTP_CONN_WANT_READ;
}

/* 클라이언트가 보낼 바이트를 덧붙인다. */
static void sess_append(sess *s, const char *data, size_t len)
{
    s->send = realloc(s->send, s->send_len + len);
    setup_or_die(s->send != NULL, "realloc");
    memcpy(s->send + s->send_len, data, len);
    s->send_len += len;
}

/* 루프의 정리 순서처럼 자원을 풀고 서버 쪽 소켓을 닫는다(클라이언트가 EOF를 본다). */
static void sess_reap(sess *s)
{
    if (s->c->fd >= 0) {
        http_conn_release(s->c);
        close(s->c->fd);
        s->c->fd = -1;
    }
}

static void sess_close(sess *s)
{
    sess_reap(s);
    close(s->client);
    if (peer >= 0) {
        close(peer);
        peer = -1;
    }
    free(s->c);
    free(s->reply);
    free(s->send);
}

static void pump_client_and_peer(sess *s)
{
    ssize_t n;

    if (s->sent < s->send_len) {
        n = write(s->client, s->send + s->sent, s->send_len - s->sent);
        if (n > 0) {
            s->sent += (size_t)n;
        }
    }
    if (s->shut_after_send && !s->shut_done && s->sent == s->send_len) {
        shutdown(s->client, SHUT_WR);
        s->shut_done = 1;
    }
    while (!s->client_eof && s->reply_len < REPLY_MAX - 1) {
        n = read(s->client, s->reply + s->reply_len, REPLY_MAX - 1 - s->reply_len);
        if (n > 0) {
            s->reply_len += (size_t)n;
        } else {
            s->client_eof = n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
            break;
        }
    }
    s->reply[s->reply_len] = '\0';
    if (s->peer_seen != peer_gen) { /* 새 백엔드: 받은 바이트를 새로 센다 */
        s->peer_seen = peer_gen;
        s->be_got_len = s->be_got_total = 0;
        s->be_got[0] = '\0';
        s->peer_eof = 0;
    }
    while (peer >= 0 && s->peer_reading && !s->peer_eof) {
        char buf[65536];
        n = read(peer, buf, sizeof(buf));
        if (n <= 0) {
            s->peer_eof = n == 0;
            break;
        }
        if (s->be_got_len < sizeof(s->be_got) - 1) {
            size_t k = (size_t)n < sizeof(s->be_got) - 1 - s->be_got_len ? (size_t)n : sizeof(s->be_got) - 1 - s->be_got_len;
            memcpy(s->be_got + s->be_got_len, buf, k);
            s->be_got_len += k;
            s->be_got[s->be_got_len] = '\0';
        }
        s->be_got_total += (size_t)n;
    }
}

/* 루프의 한 회차: 현재 기다리는 소켓만 보고, 깨운 소켓마다 한 번 실행한다. 실행 가능이면 내부 실행. */
static void sess_round(sess *s)
{
    struct pollfd p[2];
    int np = 0, ci = -1, bi = -1, i;
    short be_events = 0;

    pump_client_and_peer(s);
    if (s->finished) {
        return;
    }
    if (s->step == HTTP_CONN_RUNNABLE) {
        s->step = http_conn_run(s->c, HTTP_CONN_WAKE_INTERNAL, now_ns());
        s->finished = s->step == HTTP_CONN_FINISHED;
        return;
    }
    if (s->step == HTTP_CONN_WANT_READ || s->step == HTTP_CONN_WANT_WRITE) {
        p[np].fd = s->c->fd;
        p[np].events = s->step == HTTP_CONN_WANT_READ ? POLLIN : POLLOUT;
        ci = np++;
    }
    if (s->step == HTTP_CONN_WANT_BACKEND_READ || (s->step == HTTP_CONN_WANT_READ && s->c->uploading)) {
        be_events = POLLIN;
    } else if (s->step == HTTP_CONN_WANT_BACKEND_WRITE) {
        be_events = (short)(POLLOUT | (s->c->uploading ? POLLIN : 0));
    }
    if (be_events != 0 && s->c->be_fd >= 0) {
        p[np].fd = s->c->be_fd;
        p[np].events = be_events;
        bi = np++;
    }
    for (i = 0; i < np; i++) {
        p[i].revents = 0;
    }
    if (np == 0 || poll(p, (nfds_t)np, 10) <= 0) {
        return;
    }
    if (bi >= 0 && p[bi].revents != 0) {
        int wake = (p[bi].revents & POLLIN ? HTTP_CONN_WAKE_BACKEND_IN : 0) |
                   (p[bi].revents & POLLOUT ? HTTP_CONN_WAKE_BACKEND_OUT : 0) |
                   (p[bi].revents & (POLLERR | POLLHUP) ? HTTP_CONN_WAKE_BACKEND_ERR : 0);
        s->step = http_conn_run(s->c, wake, now_ns());
        if ((s->finished = s->step == HTTP_CONN_FINISHED)) {
            return;
        }
    }
    if (ci >= 0 && p[ci].revents != 0) {
        s->step = http_conn_run(s->c, HTTP_CONN_WAKE_CLIENT, now_ns());
        s->finished = s->step == HTTP_CONN_FINISHED;
    }
}

typedef int (*cond_fn)(sess *s);

/* cond가 참이 되거나 연결이 끝날 때까지(최대 3초) 돈다. cond 결과를 돌려준다. */
static int sess_until(sess *s, cond_fn cond)
{
    long long limit = now_ns() + 3 * SEC;
    while (now_ns() < limit) {
        sess_round(s);
        if (s->finished) {
            int i;
            sess_reap(s);
            for (i = 0; i < 100 && !s->client_eof; i++) {
                pump_client_and_peer(s);
            }
            return cond != NULL && cond(s);
        }
        if (cond != NULL && cond(s)) {
            return 1;
        }
    }
    return 0;
}

/* 연결이 끝날 때까지 돈다. */
static int is_done(sess *s)
{
    return s->finished;
}

/* ms 동안 돈다(아무 일도 없어야 하는 구간). */
static void sess_for(sess *s, int ms)
{
    long long limit = now_ns() + (long long)ms * 1000000;
    while (now_ns() < limit && !s->finished) {
        sess_round(s);
    }
    pump_client_and_peer(s);
}

static int be_has_head(sess *s)
{
    return strstr(s->be_got, "\r\n\r\n") != NULL;
}

/* 다음 백엔드 요청 머리를 기다린다(지금까지 받은 바이트는 지운다). */
static int next_head(sess *s)
{
    s->be_got_len = 0;
    s->be_got[0] = '\0';
    return sess_until(s, be_has_head);
}

static void peer_write(const char *data)
{
    size_t len = strlen(data);
    setup_or_die(peer >= 0 && write(peer, data, len) == (ssize_t)len, "peer write");
}

static size_t count_of(const char *hay, const char *needle)
{
    size_t n = 0;
    for (; (hay = strstr(hay, needle)) != NULL; hay += strlen(needle)) {
        n++;
    }
    return n;
}

static const char static_ok_head[] = "HTTP/1.1 200 OK\r\n";

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* ---- 시험 ---- */

/* TC-6-08·10: 요청 다시 쓰기, 길이 있는 응답 중계(넘는 바이트 버림), 1.1 유지 뒤 같은 연결의 정적 요청. */
static void test_length_keepalive_pipeline(void)
{
    static const char req[] = "GET /api/users?x=1 HTTP/1.1\r\nHost: c\r\nConnection: X-Hop\r\nX-Hop: 1\r\nAccept: */*\r\n\r\n"
                              "GET /index.html HTTP/1.1\r\nHost: c\r\n\r\n";
    static const char want_be[] = "GET /users?x=1 HTTP/1.0\r\nAccept: */*\r\nHost: 127.0.0.1:8001\r\nConnection: close\r\n\r\n";
    static const char want_first[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nServer: b\r\n\r\nhello";
    int fd_before = lowest_free_fd();
    sess s;

    use_routes("route /api/ 127.0.0.1:8001\n");
    connect_mode = CONNECT_OK;
    sess_open(&s, req, sizeof(req) - 1, 1);
    sess_until(&s, be_has_head);
    check("TC-6-08 백엔드 요청: HTTP/1.0, 전달 주소, 홉·Host 빼고 Host·Connection: close",
          strcmp(s.be_got, want_be) == 0, s.be_got);
    peer_write("HTTP/1.0 200 OK\r\nContent-Length: 5\r\nConnection: close\r\nServer: b\r\n\r\nhelloEXTRA");
    sess_until(&s, is_done);
    check("TC-6-09 길이만큼만 중계(EXTRA 버림), 버전 1.1, 홉 머리 빠짐, 1.1 유지라 Connection 없음",
          strncmp(s.reply, want_first, sizeof(want_first) - 1) == 0 && strstr(s.reply, "EXTRA") == NULL, s.reply);
    check("TC-6-10 유지된 연결에서 다음 정적 요청 200",
          strncmp(s.reply + sizeof(want_first) - 1, static_ok_head, sizeof(static_ok_head) - 1) == 0 &&
              strstr(s.reply, index_body) != NULL,
          s.reply);
    check("TC-6-10 결과 DONE, 백엔드 소켓 닫힘", s.finished && s.c->result == HTTP_CONN_DONE && s.c->be_fd == -1, "다르다");
    sess_close(&s);
    check("TC-6-10 fd 누수 없음", lowest_free_fd() == fd_before, "fd가 남았다");
}

/* TC-6-10: 길이 없는 응답은 EOF까지 중계하고 Connection: close 뒤 닫는다(다음 요청은 처리하지 않음). */
static void test_eof_response_closes(void)
{
    static const char req[] = "GET /api/ HTTP/1.1\r\nHost: c\r\n\r\nGET /index.html HTTP/1.1\r\nHost: c\r\n\r\n";
    sess s;

    use_routes("route /api/ h:1\n");
    sess_open(&s, req, sizeof(req) - 1, 0);
    sess_until(&s, be_has_head);
    peer_write("HTTP/1.0 200 OK\r\nX: y\r\n\r\nab");
    peer_write("c");
    close(peer);
    peer = -1;
    sess_until(&s, is_done);
    check("TC-6-10 EOF 응답: Connection: close 넣고 EOF까지 중계",
          strcmp(s.reply, "HTTP/1.1 200 OK\r\nX: y\r\nConnection: close\r\n\r\nabc") == 0, s.reply);
    check("TC-6-10 EOF 응답 뒤 닫음(DONE, 클라이언트 EOF, 다음 요청 응답 없음)",
          s.c->result == HTTP_CONN_DONE && s.client_eof && count_of(s.reply, "HTTP/1.1") == 1, "다르다");
    sess_close(&s);
}

/* TC-6-09: 204(뒤 바이트·Content-Length 버림), 304(Content-Length 유지), Content-Length: 0 — 백엔드 EOF를 기다리지 않는다. */
static void test_no_body_statuses(void)
{
    static const char req[] = "GET /api/a HTTP/1.1\r\nHost: c\r\n\r\nGET /api/b HTTP/1.1\r\nHost: c\r\n\r\n"
                              "GET /api/c HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /index.html HTTP/1.1\r\nHost: c\r\n\r\n";
    static const char want[] = "HTTP/1.1 204 No Content\r\n\r\n"
                               "HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n"
                               "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n"
                               "HTTP/1.1 200 OK\r\n";
    sess s;
    int gen;

    use_routes("route /api/ h:1\n");
    sess_open(&s, req, sizeof(req) - 1, 1);
    sess_until(&s, be_has_head);
    gen = peer_gen;
    peer_write("HTTP/1.0 204 No Content\r\nContent-Length: 5\r\n\r\nhello"); /* 백엔드는 닫지 않는다 */
    next_head(&s);
    check("TC-6-09 204 뒤 백엔드 EOF를 기다리지 않고 프록시가 먼저 닫은 뒤 다음 요청 진행",
          peer_gen == gen + 1 && replaced_peer_eof, "다음 백엔드로 가지 않았거나 이전 백엔드가 열려 있다");
    peer_write("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n0123456789");
    next_head(&s);
    peer_write("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
    sess_until(&s, is_done);
    check("TC-6-09 204 Content-Length 뺌·본문 없음, 304 Content-Length 유지·본문 없음, 길이 0, 1.0 keep-alive 유지 뒤 정적",
          strncmp(s.reply, want, sizeof(want) - 1) == 0 && strstr(s.reply, "hello") == NULL && strstr(s.reply, "0123") == NULL,
          s.reply);
    sess_close(&s);
}

/* TC-6-09: 커밋 뒤 길이 전 EOF → 두 번째 응답 없이 닫음(ERROR, EIO). */
static void test_short_body_after_commit(void)
{
    static const char req[] = "GET /api/ HTTP/1.1\r\nHost: c\r\n\r\n";
    sess s;

    use_routes("route /api/ h:1\n");
    sess_open(&s, req, sizeof(req) - 1, 0);
    sess_until(&s, be_has_head);
    peer_write("HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\nabc");
    close(peer);
    peer = -1;
    sess_until(&s, is_done);
    check("TC-6-09 길이 전 EOF: 받은 만큼만 보내고 ERROR(EIO)로 닫음",
          strcmp(s.reply, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc") == 0 && s.c->result == HTTP_CONN_ERROR &&
              s.c->err == EIO && s.client_eof,
          s.reply);
    sess_close(&s);
}

/* 백엔드 응답을 주고 클라이언트가 받은 것을 돌려준다(요청 하나 + 다음 정적 요청). */
static void run_bad_backend(const char *name, const char *backend_reply, int close_backend, const char *want_prefix)
{
    static const char req[] = "GET /api/ HTTP/1.1\r\nHost: c\r\n\r\nGET /index.html HTTP/1.1\r\nHost: c\r\n\r\n";
    char label[200];
    sess s;

    use_routes("route /api/ h:1\n");
    sess_open(&s, req, sizeof(req) - 1, 1);
    sess_until(&s, be_has_head);
    peer_write(backend_reply);
    if (close_backend) {
        close(peer);
        peer = -1;
    }
    sess_until(&s, is_done);
    snprintf(label, sizeof(label), "TC-6-11 %s → 502, 백엔드 바이트 안 보냄, 유지 뒤 정적 200", name);
    check(label,
          strncmp(s.reply, want_prefix, strlen(want_prefix)) == 0 && strstr(s.reply, "HTTP/1.1 200 OK\r\n") != NULL &&
              strstr(s.reply, "chunked") == NULL && strstr(s.reply, "HTTP/1.0") == NULL && s.c->result == HTTP_CONN_DONE,
          s.reply);
    sess_close(&s);
}

static void test_bad_responses(void)
{
    run_bad_backend("Transfer-Encoding 응답", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 0,
                    "HTTP/1.1 502 Bad Gateway\r\n");
    run_bad_backend("머리 없이 EOF", "HTTP/1.0 200 OK\r\nX: y\r\n", 1, "HTTP/1.1 502 Bad Gateway\r\n");
    run_bad_backend("빈 응답 EOF", "", 1, "HTTP/1.1 502 Bad Gateway\r\n");
    run_bad_backend("1xx", "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n\r\n", 0, "HTTP/1.1 502 Bad Gateway\r\n");
}

/* TC-6-11: connect 즉시 실패·socket 실패·EINPROGRESS 뒤 SO_ERROR 실패 → 502, 즉시 성공·EINPROGRESS 뒤 성공 → 중계. */
static void run_connect_case(const char *name, int mode, int err, int sock_fail, int reaches, const char *want_prefix)
{
    static const char req[] = "GET /api/ HTTP/1.1\r\nHost: c\r\n\r\n";
    char label[200];
    sess s;

    use_routes("route /api/ h:1\n");
    connect_mode = mode;
    so_error = err;
    socket_fail = sock_fail;
    sess_open(&s, req, sizeof(req) - 1, 1);
    if (reaches && sess_until(&s, be_has_head)) {
        peer_write("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");
    }
    sess_until(&s, is_done);
    snprintf(label, sizeof(label), "TC-6-11 %s → %s", name, strncmp(want_prefix, "HTTP/1.1 502", 12) == 0 ? "502" : "200 중계");
    check(label, strncmp(s.reply, want_prefix, strlen(want_prefix)) == 0 && s.c->result == HTTP_CONN_DONE && s.c->be_fd == -1,
          s.reply);
    sess_close(&s);
    connect_mode = CONNECT_OK;
    so_error = 0;
    socket_fail = 0;
}

static void test_connect_outcomes(void)
{
    int fd_before = lowest_free_fd();
    run_connect_case("connect 즉시 성공", CONNECT_OK, 0, 0, 1, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    run_connect_case("EINPROGRESS 뒤 성공", CONNECT_INPROGRESS, 0, 0, 1, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    run_connect_case("connect 즉시 실패(ECONNREFUSED)", CONNECT_REFUSED, 0, 0, 0, "HTTP/1.1 502 Bad Gateway\r\n");
    run_connect_case("EINPROGRESS 뒤 SO_ERROR 거부", CONNECT_INPROGRESS, ECONNREFUSED, 0, 0, "HTTP/1.1 502 Bad Gateway\r\n");
    run_connect_case("socket 생성 실패", CONNECT_OK, 0, 1, 0, "HTTP/1.1 502 Bad Gateway\r\n");
    check("TC-6-11 실패 경로 뒤 fd 누수 없음", lowest_free_fd() == fd_before, "fd가 남았다");
}

/* 연결 하나를 직접 만든다(가짜 시계 시험). client는 시험 쪽 끝. */
static http_conn *direct_open(const char *req, int *client, long long now)
{
    http_conn *c = malloc(sizeof(*c));
    int fds[2];

    setup_or_die(c != NULL && socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "준비");
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);
    if (req != NULL) {
        setup_or_die(write(fds[0], req, strlen(req)) == (ssize_t)strlen(req), "write");
    }
    peer = -1;
    http_conn_init(c, fds[1], root, &routes, NULL, now);
    *client = fds[0];
    return c;
}

static void direct_close(http_conn *c, int client)
{
    http_conn_release(c);
    close(c->fd);
    close(client);
    if (peer >= 0) {
        close(peer);
        peer = -1;
    }
    free(c);
}

/* RUNNABLE이 아닐 때까지 내부 실행을 반복한다. */
static http_conn_step run_internal(http_conn *c, http_conn_step step, long long now)
{
    while (step == HTTP_CONN_RUNNABLE) {
        step = http_conn_run(c, HTTP_CONN_WAKE_INTERNAL, now);
    }
    return step;
}

static size_t read_now(int fd, char *buf, size_t cap)
{
    ssize_t n = read(fd, buf, cap - 1);
    buf[n > 0 ? n : 0] = '\0';
    return n > 0 ? (size_t)n : 0;
}

/* TC-6-13: EINPROGRESS 연결 대기는 현재 백엔드의 쓰기·ERR 알림에서만 끝난다. 다른 깨움은 상태·마감을 바꾸지 않고, 5초 고정 마감에 504. */
static void test_connect_wait_wakes_and_504(void)
{
    int client;
    http_conn *c;
    http_conn_step step;
    long long deadline;
    char buf[4096], got[64];
    ssize_t n;

    use_routes("route /api/ h:1\n");
    connect_mode = CONNECT_INPROGRESS;
    c = direct_open("GET /api/ HTTP/1.1\r\nHost: c\r\n\r\nGET /index.html HTTP/1.1\r\nHost: c\r\n\r\n", &client, 0);
    step = run_internal(c, http_conn_run(c, HTTP_CONN_WAKE_CLIENT, 0), 0);
    deadline = c->deadline;
    check("TC-6-13 EINPROGRESS → 백엔드 쓰기 대기, 마감 시작+5초", step == HTTP_CONN_WANT_BACKEND_WRITE && deadline == 5 * SEC,
          "다르다");
    step = http_conn_run(c, HTTP_CONN_WAKE_INTERNAL, 1 * SEC);
    step = step == HTTP_CONN_WANT_BACKEND_WRITE ? http_conn_run(c, HTTP_CONN_WAKE_CLIENT, 2 * SEC) : step;
    step = step == HTTP_CONN_WANT_BACKEND_WRITE ? http_conn_run(c, HTTP_CONN_WAKE_BACKEND_IN, 3 * SEC) : step;
    n = read(peer, got, sizeof(got));
    check("TC-6-13 내부·클라이언트·백엔드 읽기 깨움은 연결 완료로 보지 않음(요청 미송신, 마감 그대로)",
          step == HTTP_CONN_WANT_BACKEND_WRITE && c->deadline == deadline && n < 0, "상태·마감이 바뀌었다");
    check("TC-6-13 마감 전에는 만료 아님", !http_conn_expired(c, 5 * SEC - 1), "만료됐다");
    step = http_conn_expire(c);
    check("TC-6-13 연결 대기 만료 → 백엔드 닫고 RUNNABLE(504 순서)", step == HTTP_CONN_RUNNABLE && c->be_fd == -1, "다르다");
    step = run_internal(c, step, 5 * SEC);
    read_now(client, buf, sizeof(buf));
    check("TC-6-13 504 Gateway Timeout 응답, 1.1 유지(Connection 없음) 뒤 같은 연결의 다음 요청 200",
          starts_with(buf, "HTTP/1.1 504 Gateway Timeout\r\n") && strstr(buf, "Connection:") == NULL &&
              strstr(buf, static_ok_head) != NULL && step == HTTP_CONN_WANT_READ,
          buf);
    direct_close(c, client);
    connect_mode = CONNECT_OK;
}

/* TC-6-11·13: 응답 머리 대기 무진행 5초 → 504. 준비 알림만으로 마감을 늘리지 않는다. */
static void test_head_wait_504(void)
{
    int client;
    http_conn *c;
    http_conn_step step;
    char buf[4096];
    int i;

    use_routes("route /api/ h:1\n");
    c = direct_open("POST /api/x HTTP/1.1\r\nHost: c\r\nContent-Length: 10\r\n\r\n0123456789", &client, 0);
    step = http_conn_run(c, HTTP_CONN_WAKE_CLIENT, 0);
    for (i = 0; i < 10 && step != HTTP_CONN_WANT_BACKEND_READ; i++) {
        step = http_conn_run(c, HTTP_CONN_WAKE_BACKEND_OUT, 0);
    }
    check("TC-6-11 업로드 끝 → 응답 머리 대기, 마감 0+5초", step == HTTP_CONN_WANT_BACKEND_READ && c->deadline == 5 * SEC,
          "다르다");
    step = http_conn_run(c, HTTP_CONN_WAKE_BACKEND_IN, 4 * SEC); /* 바이트 없는 알림 */
    check("TC-6-13 바이트 없는 알림은 마감을 늘리지 않음", step == HTTP_CONN_WANT_BACKEND_READ && c->deadline == 5 * SEC,
          "마감이 바뀌었다");
    step = http_conn_expire(c);
    step = run_internal(c, step, 5 * SEC);
    read_now(client, buf, sizeof(buf));
    check("TC-6-11 응답 대기 만료 → 504, 유지", starts_with(buf, "HTTP/1.1 504 Gateway Timeout\r\n") &&
                                                  step == HTTP_CONN_WANT_READ && c->be_fd == -1,
          buf);
    direct_close(c, client);
}

/* TC-6-11: 남은 본문을 다 버린 뒤에 502를 보내고 유지한다. */
static void test_502_after_body_discard(void)
{
    static const char head[] = "POST /api/x HTTP/1.1\r\nHost: c\r\nContent-Length: 10\r\n\r\n0123";
    static const char rest[] = "456789GET /index.html HTTP/1.1\r\nHost: c\r\n\r\n";
    sess s;
    int early;

    use_routes("route /api/ h:1\n");
    connect_mode = CONNECT_REFUSED;
    sess_open(&s, head, sizeof(head) - 1, 0);
    sess_for(&s, 300); /* 남은 본문을 기다리는 구간 */
    early = strstr(s.reply, "502") != NULL;
    s.shut_after_send = 1;
    sess_append(&s, rest, sizeof(rest) - 1);
    sess_until(&s, is_done);
    check("TC-6-11 본문이 남아 있는 동안 502를 보내지 않음", !early, s.reply);
    check("TC-6-11 남은 본문을 버린 뒤 502, 유지 뒤 다음 요청 200",
          strncmp(s.reply, "HTTP/1.1 502 Bad Gateway\r\n", 26) == 0 && strstr(s.reply, static_ok_head) != NULL &&
              s.c->result == HTTP_CONN_DONE,
          s.reply);
    sess_close(&s);
    connect_mode = CONNECT_OK;
}

/* TC-6-08·12: Expect 100-continue는 서버가 먼저 보내고, 백엔드에는 Expect 없이 본문과 검증한 길이를 보낸다. */
static void test_post_continue_forward(void)
{
    static const char req[] = "POST /api/echo HTTP/1.1\r\nHost: c\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n";
    sess s;

    use_routes("route /api/ h:1\n");
    sess_open(&s, req, sizeof(req) - 1, 0);
    sess_for(&s, 200);
    check("TC-6-08 100 Continue를 본문 전에 보냄", strcmp(s.reply, "HTTP/1.1 100 Continue\r\n\r\n") == 0, s.reply);
    s.shut_after_send = 1;
    sess_append(&s, "hello", 5);
    sess_for(&s, 200);
    check("TC-6-08 POST 본문 조각 전달, Expect 빠짐, Content-Length 다시 씀",
          strcmp(s.be_got, "POST /echo HTTP/1.0\r\nHost: h:1\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello") == 0,
          s.be_got);
    peer_write("HTTP/1.0 201 Created\r\nContent-Length: 2\r\n\r\nok");
    sess_until(&s, is_done);
    check("TC-6-08 응답 중계", strstr(s.reply, "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok") != NULL, s.reply);
    sess_close(&s);
}

/* TC-6-12: 업로드 중 조기 413 — (가) 백엔드 쓰기 막힘, (나) 클라이언트 업로드 멈춤. 응답 뒤 남은 본문을 버리고 유지한다. */
static void test_early_response(void)
{
    char head[256];
    char *body = malloc(HTTP_CONTENT_LENGTH_MAX);
    static const char next[] = "GET /index.html HTTP/1.1\r\nHost: c\r\n\r\n";
    static const char resp[] = "HTTP/1.0 413 Content Too Large\r\nContent-Length: 4\r\n\r\nbig!";
    static const char want[] = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 4\r\n\r\nbig!";
    size_t sent_at_reply;
    sess s;

    setup_or_die(body != NULL, "malloc");
    memset(body, 'b', HTTP_CONTENT_LENGTH_MAX);
    snprintf(head, sizeof(head), "POST /api/up HTTP/1.1\r\nHost: c\r\nContent-Length: %d\r\n\r\n", HTTP_CONTENT_LENGTH_MAX);
    use_routes("route /api/ h:1\n");

    /* (가) 백엔드는 읽지 않아 업로드가 쓰기 막힘에 걸린 뒤 413을 보낸다 */
    sess_open(&s, head, strlen(head), 1);
    sess_append(&s, body, HTTP_CONTENT_LENGTH_MAX);
    sess_append(&s, next, sizeof(next) - 1);
    s.peer_reading = 0;
    sess_for(&s, 500);
    check("TC-6-12 (가) 준비: 백엔드 쓰기 막힘(업로드 중 백엔드 쓰기 대기)",
          s.step == HTTP_CONN_WANT_BACKEND_WRITE && s.c->uploading, "막히지 않았다");
    peer_write(resp);
    sess_until(&s, is_done);
    check("TC-6-12 (가) 쓰기 막힘 중 조기 413 중계 → 남은 본문 버림 → 다음 요청 200",
          strncmp(s.reply, want, sizeof(want) - 1) == 0 &&
              strncmp(s.reply + sizeof(want) - 1, static_ok_head, sizeof(static_ok_head) - 1) == 0 &&
              s.c->result == HTTP_CONN_DONE && s.sent == s.send_len,
          s.reply);
    sess_close(&s);

    /* (나) 클라이언트가 본문 100바이트만 보내고 멈춘 사이 백엔드가 413을 보낸다 */
    sess_open(&s, head, strlen(head), 0);
    sess_append(&s, body, 100);
    sess_until(&s, be_has_head);
    peer_write(resp);
    sess_for(&s, 300);
    sent_at_reply = s.sent;
    check("TC-6-12 (나) 업로드가 멈춘 동안 413 도착", strncmp(s.reply, want, sizeof(want) - 1) == 0 &&
                                                         sent_at_reply == strlen(head) + 100,
          s.reply);
    s.shut_after_send = 1;
    sess_append(&s, body, HTTP_CONTENT_LENGTH_MAX - 100);
    sess_append(&s, next, sizeof(next) - 1);
    sess_until(&s, is_done);
    check("TC-6-12 (나) 나머지 본문 버린 뒤 다음 요청 200, DONE",
          strncmp(s.reply + sizeof(want) - 1, static_ok_head, sizeof(static_ok_head) - 1) == 0 &&
              s.c->result == HTTP_CONN_DONE,
          s.reply);
    sess_close(&s);
    free(body);
}

/* TC-6-12: 쓰기 실패(EPIPE) 뒤에도 백엔드가 이미 보낸 응답 머리를 읽어 중계한다. */
static void test_write_failure_then_response(void)
{
    int client, i;
    http_conn *c;
    http_conn_step step;
    char buf[8192];
    char head[256];

    use_routes("route /api/ h:1\n");
    snprintf(head, sizeof(head), "POST /api/up HTTP/1.1\r\nHost: c\r\nContent-Length: %d\r\n\r\n", HTTP_CONTENT_LENGTH_MAX);
    c = direct_open(head, &client, 0);
    step = run_internal(c, http_conn_run(c, HTTP_CONN_WAKE_CLIENT, 0), 0); /* 요청 머리를 백엔드에 보낸 뒤 본문 대기 */
    setup_or_die(peer >= 0 && step == HTTP_CONN_WANT_READ && c->uploading, "업로드 준비");
    peer_write("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");
    close(peer); /* 받은 요청을 읽지 않고 닫는다: 다음 write는 EPIPE */
    peer = -1;
    setup_or_die(write(client, "0123456789", 10) == 10, "본문 일부");
    step = http_conn_run(c, HTTP_CONN_WAKE_CLIENT, 1 * SEC); /* 본문을 읽고 백엔드 write → EPIPE */
    check("TC-6-12 쓰기 실패 → 업로드 멈추고 응답 머리 대기(502로 끝내지 않음)",
          step == HTTP_CONN_WANT_BACKEND_READ && !c->uploading && c->be_fd >= 0, "다르다");
    step = http_conn_run(c, HTTP_CONN_WAKE_BACKEND_IN | HTTP_CONN_WAKE_BACKEND_ERR, 1 * SEC);
    for (i = 0; i < 10 && step == HTTP_CONN_RUNNABLE; i++) {
        step = http_conn_run(c, HTTP_CONN_WAKE_INTERNAL, 1 * SEC);
    }
    read_now(client, buf, sizeof(buf));
    check("TC-6-12 쓰기 실패 뒤 응답 머리 수신 → 200 중계(502 아님)",
          strcmp(buf, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok") == 0 && step == HTTP_CONN_WANT_READ, buf);
    direct_close(c, client);
}

/* TC-6-13 마감 표(D-67) 나머지 행: 업로드 쓰기 막힘 → 504(남은 본문 버린 뒤), 커밋 뒤 무진행 → ERROR(ETIMEDOUT), 버리기 중 만료 → 502 미송신. */
static void test_deadline_rows(void)
{
    char *body = malloc(HTTP_CONTENT_LENGTH_MAX), head[256], buf[4096];
    static const char next[] = "GET /index.html HTTP/1.1\r\nHost: c\r\n\r\n";
    http_conn_step step;
    http_conn *c;
    int client;
    sess s;

    setup_or_die(body != NULL, "malloc");
    memset(body, 'b', HTTP_CONTENT_LENGTH_MAX);
    snprintf(head, sizeof(head), "POST /api/up HTTP/1.1\r\nHost: c\r\nContent-Length: %d\r\n\r\n", HTTP_CONTENT_LENGTH_MAX);
    use_routes("route /api/ h:1\n");

    sess_open(&s, head, strlen(head), 1);
    sess_append(&s, body, HTTP_CONTENT_LENGTH_MAX);
    sess_append(&s, next, sizeof(next) - 1);
    s.peer_reading = 0;
    sess_for(&s, 300);
    step = s.step == HTTP_CONN_WANT_BACKEND_WRITE ? http_conn_expire(s.c) : HTTP_CONN_FINISHED;
    check("TC-6-13 업로드 쓰기 막힘 만료 → 백엔드 닫고 RUNNABLE", step == HTTP_CONN_RUNNABLE && s.c->be_fd == -1, "다르다");
    s.step = step;
    sess_until(&s, is_done);
    check("TC-6-13 쓰기 막힘 504는 남은 본문을 다 버린 뒤, 유지 뒤 다음 요청 200",
          starts_with(s.reply, "HTTP/1.1 504 Gateway Timeout\r\n") && strstr(s.reply, static_ok_head) != NULL &&
              s.sent == s.send_len && s.c->result == HTTP_CONN_DONE,
          s.reply);
    sess_close(&s);
    free(body);

    c = direct_open("GET /api/ HTTP/1.1\r\nHost: c\r\n\r\n", &client, 0);
    step = run_internal(c, http_conn_run(c, HTTP_CONN_WAKE_CLIENT, 0), 0);
    peer_write("HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\nabc");
    step = run_internal(c, http_conn_run(c, HTTP_CONN_WAKE_BACKEND_IN, 1 * SEC), 1 * SEC);
    step = run_internal(c, http_conn_run(c, HTTP_CONN_WAKE_BACKEND_IN, 1 * SEC), 1 * SEC);
    read_now(client, buf, sizeof(buf));
    check("TC-6-13 준비: 커밋 뒤 본문 대기(백엔드 읽기), 마감 마지막 진행+5초",
          step == HTTP_CONN_WANT_BACKEND_READ && c->committed && c->deadline == 6 * SEC &&
              strcmp(buf, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc") == 0,
          buf);
    step = http_conn_expire(c);
    check("TC-6-13 커밋 뒤 무진행 만료 → 응답 없이 ERROR(ETIMEDOUT)",
          step == HTTP_CONN_FINISHED && c->result == HTTP_CONN_ERROR && c->err == ETIMEDOUT, "다르다");
    direct_close(c, client);

    connect_mode = CONNECT_REFUSED;
    c = direct_open("POST /api/x HTTP/1.1\r\nHost: c\r\nContent-Length: 10\r\n\r\n0123", &client, 0);
    step = run_internal(c, http_conn_run(c, HTTP_CONN_WAKE_CLIENT, 0), 0);
    step = step == HTTP_CONN_WANT_READ ? http_conn_run(c, HTTP_CONN_WAKE_CLIENT, 1 * SEC) : step;
    check("TC-6-13 준비: 502 대기 중 본문 버리기(클라이언트 읽기), 진행 없는 깨움은 마감 그대로",
          step == HTTP_CONN_WANT_READ && c->deadline == 5 * SEC, "다르다");
    step = http_conn_expire(c);
    check("TC-6-13 버리기 중 만료 → TIMED_OUT, 저장한 502 보내지 않음",
          step == HTTP_CONN_FINISHED && c->result == HTTP_CONN_TIMED_OUT && read_now(client, buf, sizeof(buf)) == 0, buf);
    direct_close(c, client);
    connect_mode = CONNECT_OK;
}

/* TC-6-05·10: 규칙 밖(/api, -c 없음)은 정적 처리, 규칙 안 POST는 405 판정보다 먼저 프록시, 요청마다 라운드 로빈. */
static void test_matching_in_conn(void)
{
    static const char req[] = "GET /api HTTP/1.1\r\nHost: c\r\n\r\nGET /api/r HTTP/1.1\r\nHost: c\r\n\r\n";
    route_table *saved = NULL;
    int first_8001;
    sess s;

    use_routes("route /api/ h:8001 h:8002\n");
    sess_open(&s, req, sizeof(req) - 1, 1);
    sess_until(&s, be_has_head);
    check("TC-6-05 /api 는 정적 404", strncmp(s.reply, "HTTP/1.1 404 Not Found\r\n", 24) == 0, s.reply);
    first_8001 = strstr(s.be_got, "Host: h:8001\r\n") != NULL;
    peer_write("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
    sess_until(&s, is_done);
    sess_close(&s);
    {
        static const char post[] = "POST /api/r HTTP/1.1\r\nHost: c\r\nContent-Length: 0\r\n\r\n";
        sess_open(&s, post, sizeof(post) - 1, 1);
    }
    sess_until(&s, be_has_head);
    check("TC-6-05 요청마다 라운드 로빈(8001 → 8002), POST도 프록시",
          first_8001 && strncmp(s.be_got, "POST /r HTTP/1.0\r\n", 18) == 0 &&
              strstr(s.be_got, "Host: h:8002\r\n") != NULL,
          s.be_got);
    peer_write("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
    sess_until(&s, is_done);
    sess_close(&s);

    {
        static const char get[] = "GET /api/r HTTP/1.1\r\nHost: c\r\n\r\n";
        sess_open(&s, get, sizeof(get) - 1, 1);
    }
    s.c->routes = saved; /* -c 생략과 같다 */
    connect_calls = 0;
    sess_until(&s, is_done);
    check("TC-6-14 라우팅 표 없음: /api/r 는 정적 404, 백엔드 연결 없음",
          strncmp(s.reply, "HTTP/1.1 404 Not Found\r\n", 24) == 0 && connect_calls == 0, s.reply);
    sess_close(&s);
}

int main(void)
{
    alarm(120);
    signal(SIGPIPE, SIG_IGN); /* D-05: 서버와 같은 조건 */
    printf("test_proxy_conn (TC-6-05, TC-6-08~TC-6-14)\n");
    build_root();
    test_length_keepalive_pipeline();
    test_eof_response_closes();
    test_no_body_statuses();
    test_short_body_after_commit();
    test_bad_responses();
    test_connect_outcomes();
    test_connect_wait_wakes_and_504();
    test_head_wait_504();
    test_502_after_body_discard();
    test_post_continue_forward();
    test_early_response();
    test_write_failure_then_response();
    test_deadline_rows();
    test_matching_in_conn();
    remove_root();
    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
