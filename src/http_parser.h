#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#define HTTP_REQUEST_HEAD_MAX 8192 /* D-09, D-10: 종료 빈 줄 포함 요청 머리 최대 길이 */
#define HTTP_MAX_HEADERS      100  /* D-15 */

typedef enum {
    HTTP_PARSE_INCOMPLETE,           /* 아직 \r\n\r\n 없음, 더 읽어야 함 */
    HTTP_PARSE_OK,
    HTTP_PARSE_BAD_REQUEST,          /* 400: 문법·줄 바꿈 오류, 8KB 초과, 헤더 수 초과, Host 규칙 위반 */
    HTTP_PARSE_NOT_IMPLEMENTED,      /* 501 */
    HTTP_PARSE_VERSION_NOT_SUPPORTED /* 505 */
} http_parse_result;

typedef struct {
    const char *name;
    const char *value;
} http_header;

/* 값으로 복사하지 말고 포인터로만 넘긴다. 문자열 포인터는 이 구조체의 storage를 가리킨다. */
typedef struct {
    const char *method, *target, *version; /* storage 안의 복사본 */
    int version_minor;                     /* 0 또는 1 */
    http_header headers[HTTP_MAX_HEADERS]; /* 받은 순서 */
    size_t header_count;
    char storage[HTTP_REQUEST_HEAD_MAX];   /* 모든 복사본이 여기에 들어간다 */
} http_request;

/*
 * data[0..len)은 그 연결에서 지금까지 받은 바이트 전체다(D-14). I/O·할당·전역 상태가 없다.
 * INCOMPLETE는 len < HTTP_REQUEST_HEAD_MAX에서만 나온다. out은 OK일 때만 유효하다.
 */
http_parse_result http_parse_request(const char *data, size_t len, http_request *out);

/* 이름을 대소문자 구분 없이 비교해 첫 번째 헤더 값(앞뒤 공백 제거됨)을 돌려준다. 없으면 NULL. */
const char *http_request_header(const http_request *req, const char *name);

#endif /* HTTP_PARSER_H */
