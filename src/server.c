/*
 * 서버: docs/design/step1.md, step2.md, step3.md
 * 리슨 소켓을 준비하고, 연결을 하나씩 받아 요청 하나를 읽고 파일이나 오류 응답 하나를 보낸 뒤 닫는다.
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
#include <unistd.h>

#define DRAIN_LIMIT (16 * 1024)
#define FILE_BUF_SIZE 8192

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

/* 오류 응답을 보내요. 상태 줄과 헤더, "코드 문구\n" 본문. 200은 send_file()이 보내요 */
static int send_response(int fd, int status)
{
    char res[256];
    char body[64];
    int body_len = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
    int n = snprintf(res, sizeof(res),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     status, http_reason(status), body_len, body);
    return write_all(fd, res, (size_t)n);
}

/*
 * 파일을 열어 200 헤더와 내용을 8KB씩 보내요.
 * 0: 다 보냄, 403·404·500: 아무것도 안 보냄(오류 응답을 보낼 것), -1: 헤더를 보낸 뒤 실패(닫기만 할 것)
 */
static int send_file(int fd, const char *file)
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
                     "Connection: close\r\n"
                     "\r\n",
                     mime_type(file), (long long)st.st_size);
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

int handle_conn(struct conn *c, const char *root_real)
{
    size_t end;
    int status = 0;

    while ((end = http_find_header_end(c->buf, c->len)) == 0) {
        if (c->len == sizeof(c->buf)) {          /* 헤더의 끝 전에 버퍼가 참 */
            status = 431;
            break;
        }
        ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
        if (n == 0)
            return -1;                           /* 헤더의 끝 전에 끊김: 응답 없음 */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return -1;
        }
        c->len += (size_t)n;
    }

    if (status == 0)
        status = http_parse_request(c->buf, end, &c->req);
    if (status == 0) {
        char file[PATH_MAX];
        status = resolve_path(root_real, c->req.path, file, sizeof(file));
        if (status == 0) {
            status = send_file(c->fd, file);
            if (status < 0)
                return -1;                       /* 보내다 끊김: 오류 응답 없이 닫기 */
            if (status == 0) {
                drain(c->fd);
                return 0;
            }
        }
    }
    if (send_response(c->fd, status) < 0)
        return -1;
    drain(c->fd);
    return 0;
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
        c->fd = client_fd;
        c->len = 0;

        handle_conn(c, root_real);

        if (close(c->fd) < 0)
            perror("close");
        free(c);
    }
}
