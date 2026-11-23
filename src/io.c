#define _POSIX_C_SOURCE 200809L

#include "io.h"

#include <errno.h>
#include <unistd.h>

int io_write_all(int fd, const void *data, size_t len)
{
    const unsigned char *p = data; /* void *에는 포인터 산술을 할 수 없다(-Wpedantic) */

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}
