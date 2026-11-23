#define _POSIX_C_SOURCE 200809L

#include "echo.h"
#include "io.h"

#include <errno.h>
#include <unistd.h>

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
        if (io_write_all(fd, buffer, (size_t)n) != 0) {
            return ECHO_ERROR;
        }
    }
}
