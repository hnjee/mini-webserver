/*
 * Step 1 TCP 에코 서버: docs/design/step1.md
 * 리슨 소켓을 준비하고, 연결을 하나씩 받아 받은 바이트를 그대로 돌려준다.
 */
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 4096

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

int serve_client(int client_fd)
{
    char buf[BUF_SIZE];

    for (;;) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n == 0)
            return 0;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return -1;
        }

        ssize_t sent = 0;                       /* 받은 n바이트를 다 쓸 때까지 */
        while (sent < n) {
            ssize_t w = write(client_fd, buf + sent, (size_t)(n - sent));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                perror("write");
                return -1;
            }
            sent += w;
        }
    }
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

        serve_client(client_fd);

        if (close(client_fd) < 0)
            perror("close");
    }
}
