#ifndef RESPONSE_H
#define RESPONSE_H

#include <stddef.h>

/* 200 → "OK", 404 → "Not Found"처럼 상태 문구를 돌려준다 */
const char *status_text(int status);

/*
 * buf의 len바이트를 fd에 끝까지 쓴다. 성공하면 0, 실패하면 -1.
 * write()는 요청한 것보다 적게 쓰고 돌아올 수 있어서, 남은 만큼 반복한다.
 */
int write_all(int fd, const void *buf, size_t len);

/*
 * 상태 라인과 헤더(Date, Content-Type, Content-Length, Connection)만 보낸다.
 * 본문은 호출한 쪽이 정확히 content_length바이트를 이어서 보내야 한다.
 * 파일처럼 본문을 조각내서 보낼 때 쓴다. 성공하면 0, 실패하면 -1.
 */
int send_header(int fd, int status, const char *content_type, size_t content_length);

/*
 * 헤더 → 본문 순서로 끝까지 보낸다. Content-Length는 body_len으로 직접 쓴다.
 * 성공하면 0, 실패하면 -1.
 */
int send_response(int fd, int status, const char *content_type,
                  const char *body, size_t body_len);

/* "400 Bad Request" 같은 text/plain 본문으로 에러 응답을 보낸다 */
int send_error(int fd, int status);

#endif
