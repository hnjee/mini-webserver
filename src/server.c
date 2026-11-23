#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define ECHO_BUF_SIZE 4096

/*
 * buf의 len바이트를 fd에 끝까지 쓴다. 성공하면 0, 실패하면 -1.
 * write()는 요청한 것보다 적게 쓰고 돌아올 수 있어서, 남은 만큼 반복한다.
 */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)   /* 시그널 때문에 끊겼을 뿐이니 다시 쓴다 */
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/*
 * 연결 하나를 처리한다. 상대가 끊거나 에러가 나면 client_fd를 닫고 돌아온다.
 *   read() > 0  : 읽은 n바이트만 그대로 돌려준다
 *   read() == 0 : 상대가 연결을 끊었다 (EOF)
 *   read() < 0  : EINTR이면 다시 읽고, 그 밖에는 연결을 끝낸다
 */
static void handle_echo(int client_fd)
{
    char buf[ECHO_BUF_SIZE];

    for (;;) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n > 0) {
            if (write_all(client_fd, buf, (size_t)n) < 0) {
                perror("write");
                break;
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR)
                continue;
            perror("read");
            break;
        }
    }
    close(client_fd);
}

int server_open(int port)
{
    struct sockaddr_in addr;
    int listen_fd;
    int on = 1;

    /* 1. 소켓 만들기: 커널이 소켓을 만들고 fd 번호를 돌려준다 */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    /* 2. TIME_WAIT가 남아 있어도 bind()할 수 있게 한다. bind()보다 먼저 해야 한다 */
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(listen_fd);
        return -1;
    }

    /* 3. 포트에 묶기: 모든 주소(INADDR_ANY)에서, 네트워크 바이트 순서로 */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    /* 4. 연결 요청을 받기 시작한다. handshake가 끝난 연결은 accept 대기열에 쌓인다 */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

void server_run(int listen_fd)
{
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        char ip[INET_ADDRSTRLEN];
        int client_fd;
        int client_port;

        /* 5. accept 대기열에서 연결 하나를 꺼낸다. 연결마다 새 fd가 생긴다 */
        client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;   /* 연결 하나의 실패로 서버를 멈추지 않는다 */
        }

        if (inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip)) == NULL)
            strcpy(ip, "?");
        client_port = ntohs(client_addr.sin_port);

        printf("connected: %s:%d (fd=%d)\n", ip, client_port, client_fd);
        handle_echo(client_fd);   /* 끝나면 client_fd를 닫고 돌아온다 */
        printf("closed:    %s:%d\n", ip, client_port);
    }
}
