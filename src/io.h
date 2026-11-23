#ifndef IO_H
#define IO_H

#include <stddef.h>

/*
 * 부분 write가 생겨도 len 바이트를 모두 fd에 쓴다. EINTR은 재시도한다.
 * 성공 0, 실패 -1(errno 보존). fd는 닫지 않는다.
 */
int io_write_all(int fd, const void *data, size_t len);

#endif /* IO_H */
