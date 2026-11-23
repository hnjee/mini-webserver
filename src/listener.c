#define _POSIX_C_SOURCE 200809L

#include "listener.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* 커널 최대값(SOMAXCONN, Linux 4096)을 요청한다. 실제 값은 net.core.somaxconn으로 제한된다. */
#define LISTEN_BACKLOG SOMAXCONN

/* 실패한 단계 이름과 errno를 error에 쓰고, 열린 소켓을 닫는다. */
static int fail(int fd, const char *step, int port, char *error, size_t error_size)
{
    int saved = errno;

    snprintf(error, error_size, "%s 실패 (포트 %d): %s", step, port, strerror(saved));
    if (fd >= 0) {
        close(fd);
    }
    errno = saved;
    return -1;
}

int listener_open(int port, char *error, size_t error_size)
{
    struct sockaddr_in addr;
    int fd;
    int on = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return fail(-1, "socket", port, error, error_size);
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        return fail(fd, "setsockopt(SO_REUSEADDR)", port, error, error_size);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        return fail(fd, "bind", port, error, error_size);
    }
    if (listen(fd, LISTEN_BACKLOG) != 0) {
        return fail(fd, "listen", port, error, error_size);
    }
    return fd;
}
