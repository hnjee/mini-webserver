/*
 * 서버: docs/design/step1.md, docs/design/step2.md
 * 리슨 소켓을 준비하고, 연결을 하나씩 받아 요청 하나를 읽고 응답 하나를 보낸 뒤 닫는다.
 */
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DRAIN_LIMIT (16 * 1024)

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

/* 상태 코드로 응답 전체를 만들어 보내요. 200은 본문 없이, 오류는 "코드 문구\n" 본문과 함께 */
static int send_response(int fd, int status)
{
    char res[256];
    int n;

    if (status == 200) {
        n = snprintf(res, sizeof(res),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n"
                     "\r\n");
    } else {
        char body[64];
        int body_len = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
        n = snprintf(res, sizeof(res),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     status, http_reason(status), body_len, body);
    }
    return write_all(fd, res, (size_t)n);
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

int handle_conn(struct conn *c)
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
    if (status == 0)
        status = 200;
    if (send_response(c->fd, status) < 0)
        return -1;
    drain(c->fd);
    return 0;
}

void server_run(int listen_fd)
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

        handle_conn(c);

        if (close(c->fd) < 0)
            perror("close");
        free(c);
    }
}
