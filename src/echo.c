#define _POSIX_C_SOURCE 200809L

#include "echo.h"

#include <errno.h>
#include <unistd.h>

/* 부분 write가 생겨도 len 바이트를 모두 보낸다. 실패하면 -1(errno 보존). */
static int write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

echo_result echo_serve(int fd)
{
    char buffer[ECHO_BUFFER_SIZE];

    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n == 0) {
            return ECHO_PEER_CLOSED;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ECHO_ERROR;
        }
        if (write_all(fd, buffer, (size_t)n) != 0) {
            return ECHO_ERROR;
        }
    }
}
