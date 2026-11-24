/*
 * 서버: docs/design/step1.md ~ step4.md
 * 리슨 소켓을 준비하고, 연결을 하나씩 받아 닫을 때까지 요청을 읽고 응답하기를 반복한다 (keep-alive).
 */
#include "server.h"

#include "file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DRAIN_LIMIT (16 * 1024)
#define FILE_BUF_SIZE 8192
#define IDLE_TIMEOUT_SEC 15

int server_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(fd, buf + sent, len - sent);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            perror("write");
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

/* 오류 응답을 보내요. 본문은 "코드 문구\n", extra는 "Allow: GET" 같은 헤더 한 줄이거나 NULL */
static int send_error(int fd, int status, int keep, const char *extra)
{
    char res[512];
    char body[64];
    int body_len = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
    int n = snprintf(res, sizeof(res),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %d\r\n"
                     "%s%s"
                     "Connection: %s\r\n"
                     "\r\n"
                     "%s",
                     status, http_reason(status), body_len,
                     extra ? extra : "", extra ? "\r\n" : "",
                     keep ? "keep-alive" : "close", body);
    return write_all(fd, res, (size_t)n);
}

/*
 * 파일을 열어 200 헤더와 내용을 8KB씩 보내요.
 * 0: 다 보냄, 403·404·500: 아무것도 안 보냄(오류 응답을 보낼 것), -1: 헤더를 보낸 뒤 실패(닫기만 할 것)
 */
static int send_file(int fd, const char *file, int keep)
{
    int ffd = open(file, O_RDONLY);
    if (ffd < 0) {
        int err = errno;
        return err == ENOENT ? 404 : err == EACCES ? 403 : 500;
    }

    struct stat st;
    if (fstat(ffd, &st) < 0) {
        close(ffd);
        return 500;
    }
    if (!S_ISREG(st.st_mode)) {
        close(ffd);
        return 403;
    }

    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     mime_type(file), (long long)st.st_size, keep ? "keep-alive" : "close");
    if (write_all(fd, head, (size_t)n) < 0) {
        close(ffd);
        return -1;
    }

    char buf[FILE_BUF_SIZE];
    for (;;) {
        ssize_t r = read(ffd, buf, sizeof(buf));
        if (r == 0)
            break;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("read file");
            close(ffd);
            return -1;                           /* 헤더는 이미 나감: 닫기만 */
        }
        if (write_all(fd, buf, (size_t)r) < 0) {
            close(ffd);
            return -1;
        }
    }
    close(ffd);
    return 0;
}

/* 쓰기를 먼저 닫고(FIN), 받기 버퍼에 남은 데이터를 16KB까지 읽어 버려요. close()에서 RST가 나가지 않게 */
static void drain(int fd)
{
    char tmp[4096];
    size_t total = 0;

    if (shutdown(fd, SHUT_WR) < 0) {
        perror("shutdown");
        return;
    }
    while (total < DRAIN_LIMIT) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n == 0)
            return;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        total += (size_t)n;
    }
}

/* read()가 15초 넘게 기다리면 -1(EAGAIN)로 돌아오게 해요 (S4-13) */
static void set_read_timeout(int fd)
{
    struct timeval tv = {IDLE_TIMEOUT_SEC, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        perror("setsockopt SO_RCVTIMEO");
}

/* 버퍼에 남은 것부터 헤더의 끝까지 읽어요. 0: 찾음(*end), 431: 버퍼 가득, -1: 끊김·타임아웃·실패 */
static int read_header(struct conn *c, size_t *end)
{
    while ((*end = http_find_header_end(c->buf, c->len)) == 0) {
        if (c->len == sizeof(c->buf))
            return 431;
        ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
        if (n == 0)
            return -1;                           /* 클라이언트가 끊음 */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("read");
            return -1;                           /* 타임아웃이거나 실패 */
        }
        c->len += (size_t)n;
    }
    return 0;
}

/* 본문 길이를 정해요. 0: *cl에 길이, 그 밖: 경계를 알 수 없어 보낼 상태 코드 (400·411·413·501) */
static int body_length(const struct http_request *req, size_t *cl)
{
    if (http_find_header(req, "Transfer-Encoding") != NULL)
        return 501;
    int r = http_content_length(req, cl);
    if (r < 0)
        return 400;
    if (r == 0 && strcmp(req->method, "POST") == 0)
        return 411;
    if (*cl > HTTP_MAX_BODY)
        return 413;
    return 0;
}

/* 헤더 버퍼에 온 본문을 옮기고 모자란 만큼 더 읽어 c->body에 모아요. *consumed: 헤더 버퍼에서 이 요청이 쓴 바이트 수 */
static int read_body(struct conn *c, size_t end, size_t cl, size_t *consumed)
{
    size_t have = c->len - end;
    if (have > cl)
        have = cl;                               /* 그 뒤는 다음 요청 */
    *consumed = end + have;
    if (cl == 0)
        return 0;

    c->body = malloc(cl);
    if (c->body == NULL) {
        perror("malloc");
        return -1;
    }
    memcpy(c->body, c->buf + end, have);
    c->body_len = have;

    while (c->body_len < cl) {
        ssize_t n = read(c->fd, c->body + c->body_len, cl - c->body_len);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("read");
            return -1;
        }
        c->body_len += (size_t)n;
    }
    return 0;
}

/* 받은 본문을 그대로 돌려줘요. Content-Type은 요청 값, 없으면 application/octet-stream (S4-07) */
static int send_echo(int fd, const struct conn *c, int keep)
{
    const char *type = http_find_header(&c->req, "Content-Type");
    char head[HEADER_BUF_SIZE];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     type != NULL ? type : "application/octet-stream",
                     c->body_len, keep ? "keep-alive" : "close");
    if (write_all(fd, head, (size_t)n) < 0)
        return -1;
    if (c->body_len > 0 && write_all(fd, c->body, c->body_len) < 0)
        return -1;
    return 0;
}

/* path가 name과 같은지. ? 뒤 쿼리 문자열은 무시해요 */
static int path_is(const char *path, const char *name)
{
    size_t n = strlen(name);
    return strncmp(path, name, n) == 0 && (path[n] == '\0' || path[n] == '?');
}

/* 본문 버퍼를 해제하고 비워 둬요 */
static void free_body(struct conn *c)
{
    free(c->body);
    c->body = NULL;
    c->body_len = 0;
}

/* 경로와 메서드에 맞는 응답 하나를 보내요. 0: 보냄, -1: 보내다 끊김 */
static int respond(struct conn *c, const char *root_real, int keep)
{
    int post = strcmp(c->req.method, "POST") == 0;

    if (path_is(c->req.path, "/echo"))
        return post ? send_echo(c->fd, c, keep) : send_error(c->fd, 405, keep, "Allow: POST");
    if (post)
        return send_error(c->fd, 405, keep, "Allow: GET");

    char file[PATH_MAX];
    int status = resolve_path(root_real, c->req.path, file, sizeof(file));
    if (status == 0) {
        status = send_file(c->fd, file, keep);
        if (status <= 0)
            return status;                       /* 0: 보냄, -1: 보내다 끊김 */
    }
    return send_error(c->fd, status, keep, NULL);
}


int handle_conn(struct conn *c, const char *root_real)
{
    for (;;) {
        size_t end = 0, cl = 0, consumed = 0;
        int status = read_header(c, &end);
        if (status < 0)
            return c->len == 0 ? 0 : -1;         /* 요청 사이에서 끊김은 정상 종료, 요청 중간이면 비정상 */
        if (status == 0)
            status = http_parse_request(c->buf, end, &c->req);
        if (status == 0)
            status = body_length(&c->req, &cl);
        if (status != 0) {                       /* 경계를 모름: 닫기 */
            send_error(c->fd, status, 0, NULL);
            drain(c->fd);
            return 0;
        }
        if (read_body(c, end, cl, &consumed) < 0) {
            free_body(c);
            return -1;
        }

        int keep = http_keep_alive(&c->req);
        int r = respond(c, root_real, keep);     /* 경로별 처리, 0 또는 -1 */
        free_body(c);
        if (r < 0)
            return -1;                           /* 보내다 끊김 */
        if (!keep) {
            drain(c->fd);
            return 0;
        }

        memmove(c->buf, c->buf + consumed, c->len - consumed);
        c->len -= consumed;
    }
}

void server_run(int listen_fd, const char *root_real)
{
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }

        struct conn *c = malloc(sizeof(*c));
        if (c == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }
        set_read_timeout(client_fd);
        c->fd = client_fd;
        c->len = 0;
        c->body = NULL;
        c->body_len = 0;

        handle_conn(c, root_real);

        if (close(c->fd) < 0)
            perror("close");
        free(c);
    }
}
