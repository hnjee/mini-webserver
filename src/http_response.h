#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>

/*
 * status의 응답 전체(상태 줄, Date, Content-Type, Content-Length, Connection: close, 본문)를
 * fd에 끝까지 쓴다(D-18). Content-Length는 body_len으로 만든다. 본문은 body_len 바이트 그대로 쓴다.
 * 지원 상태: 200, 400, 501, 505. 그 밖의 코드는 아무것도 쓰지 않고 -1(EINVAL).
 * 성공 0, 실패 -1(errno 보존). fd는 닫지 않는다.
 */
int http_response_send(int fd, int status, const char *content_type,
                       const char *body, size_t body_len);

#endif /* HTTP_RESPONSE_H */
