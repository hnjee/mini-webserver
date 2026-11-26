#ifndef ACCESS_LOG_H
#define ACCESS_LOG_H

#include <stddef.h>

/* 서버 시작 시 출력 대상을 정한다. 단위 테스트에서는 호출하지 않으면 로그를 쓰지 않는다. */
void access_log_enable(void);
void access_log_write(const char *peer, const char *request, int status, size_t bytes,
                      long long started_ns, const char *result);

#endif
