#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>

/*
 * 지원 상태: 200, 301, 400, 403, 404, 405, 413, 414, 431, 500, 501, 505 (D-27, D-52).
 * 그 밖의 코드(100 포함)는 아무것도 쓰지 않고 -1(EINVAL). 100은 http_response_send_continue만 쓴다.
 * 모든 함수는 성공 0, 실패 -1(errno 보존)이고 fd는 닫지 않는다.
 */

/* 응답 Connection 머리(D-51). 값은 http_conn이 정하고 응답 모듈은 쓰기만 한다. */
typedef enum {
    HTTP_CONNECTION_NONE = 0,  /* 넣지 않음 (1.1 유지) */
    HTTP_CONNECTION_CLOSE,     /* Connection: close */
    HTTP_CONNECTION_KEEP_ALIVE /* Connection: keep-alive (1.0 유지) */
} http_connection;

/*
 * 추가 머리 묶음(D-52). NULL을 넘기면 모두 없음과 같다.
 * location은 301일 때만, allow는 405일 때만 있어야 하고 그때는 반드시 있어야 한다. 어기면 아무것도 쓰지 않고 -1(EINVAL).
 */
typedef struct {
    const char *location;
    const char *allow;
    http_connection connection;
} http_response_extra;

/*
 * 상태 줄, Date, Content-Type, Content-Length(content_length), [Location], [Allow], [Connection], 빈 줄만 쓴다.
 * 서버 코드에서는 파일 응답(fstat 크기)과 /echo(요청 본문 길이)만 이 함수를 직접 부른다(D-33, D-52).
 */
int http_response_send_head(int fd, int status, const char *content_type,
                            size_t content_length, const http_response_extra *extra);

/*
 * status의 응답 전체(머리와 본문)를 fd에 끝까지 쓴다(D-16). Content-Length는 body_len으로 만든다.
 * location·allow는 받지 않는다(있으면 -1, EINVAL). 301·405는 http_response_send_status로 보낸다.
 */
int http_response_send(int fd, int status, const char *content_type,
                       const char *body, size_t body_len, const http_response_extra *extra);

/*
 * status의 본문 "<코드> <이유 문구>\n"(text/plain; charset=utf-8)과 그 길이로 머리와 본문을 쓴다(D-33).
 * status: 200을 뺀 지원 상태. 200은 -1(EINVAL). location·allow 규칙은 send_head와 같다.
 */
int http_response_send_status(int fd, int status, const http_response_extra *extra);

/* "HTTP/1.1 100 Continue\r\n\r\n"만 쓴다(D-50, D-52). Date·Content-Length·본문은 없다. */
int http_response_send_continue(int fd);

#endif /* HTTP_RESPONSE_H */
