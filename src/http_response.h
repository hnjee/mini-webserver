#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>

/*
 * 지원 상태: 200, 301, 400, 403, 404, 500, 501, 505 (D-31). 그 밖의 코드는 아무것도 쓰지 않고 -1(EINVAL).
 * 모든 함수는 성공 0, 실패 -1(errno 보존)이고 fd는 닫지 않는다.
 */

/*
 * 상태 줄, Date, Content-Type, Content-Length(content_length), [Location], Connection: close, 빈 줄만 쓴다.
 * location은 status가 301일 때만, 그리고 301이면 반드시 NULL이 아니어야 한다. 어기면 아무것도 쓰지 않고 -1(EINVAL).
 * 서버 코드에서는 파일 응답(fstat 크기)만 이 함수를 직접 부른다(D-40).
 */
int http_response_send_head(int fd, int status, const char *content_type,
                            size_t content_length, const char *location);

/*
 * status의 응답 전체(머리와 본문)를 fd에 끝까지 쓴다(D-18). Content-Length는 body_len으로 만든다.
 * http_response_send_head(fd, status, content_type, body_len, NULL) 뒤에 본문을 쓰므로 301은 보낼 수 없다(-1, EINVAL).
 */
int http_response_send(int fd, int status, const char *content_type,
                       const char *body, size_t body_len);

/*
 * status의 본문 "<코드> <이유 문구>\n"(text/plain; charset=utf-8)과 그 길이로 머리와 본문을 쓴다(D-40).
 * status: 301, 400, 403, 404, 500, 501, 505. 200과 그 밖의 코드는 -1(EINVAL). location 규칙은 send_head와 같다.
 */
int http_response_send_status(int fd, int status, const char *location);

#endif /* HTTP_RESPONSE_H */
