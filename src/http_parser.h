#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#define HTTP_REQUEST_HEAD_MAX 8192 /* D-09, D-10: 종료 빈 줄 포함 요청 머리 최대 길이 */
#define HTTP_MAX_HEADERS      100  /* D-15 */
#define HTTP_REQUEST_TARGET_MAX 2048 /* D-43: 요청 대상(쿼리 포함) 최대 길이 */
#define HTTP_CONTENT_LENGTH_MAX 1048576 /* D-44: 본문 최대 길이 */

typedef enum {
    HTTP_PARSE_INCOMPLETE,           /* 아직 \r\n\r\n 없음, 더 읽어야 함 */
    HTTP_PARSE_OK,
    HTTP_PARSE_BAD_REQUEST,           /* 400: 문법·줄 바꿈 오류, Host 규칙 위반, 전송 코딩·Content-Length 형식 오류 */
    HTTP_PARSE_NOT_IMPLEMENTED,       /* 501: 메서드, 마지막 전송 코딩이 chunked */
    HTTP_PARSE_VERSION_NOT_SUPPORTED, /* 505 */
    HTTP_PARSE_URI_TOO_LONG,          /* 414 (D-43) */
    HTTP_PARSE_HEADER_TOO_LARGE,      /* 431: 8KB 안에 종료 빈 줄 없음, 헤더 101개 (D-42) */
    HTTP_PARSE_CONTENT_TOO_LARGE      /* 413 (D-44) */
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
    size_t head_len;                       /* OK: 종료 빈 줄 포함 머리 길이 (D-47) */
    size_t content_length;                 /* OK: 본문 길이, 없으면 0 (D-49) */
    char storage[HTTP_REQUEST_HEAD_MAX];   /* 모든 복사본이 여기에 들어간다 */
} http_request;

/*
 * data[0..len)은 현재 요청 시작부터 받은 바이트다(D-14, D-47). I/O·할당·전역 상태가 없다.
 * D-48 순서(414 → 400 줄 바꿈 → 431 → 형식 → 전송 코딩 → 길이 형식 → 413)로 첫 결과를 돌려준다.
 * INCOMPLETE는 len < HTTP_REQUEST_HEAD_MAX에서만 나온다. out은 OK일 때만 유효하다.
 */
http_parse_result http_parse_request(const char *data, size_t len, http_request *out);

/* 이름을 대소문자 구분 없이 비교해 첫 번째 헤더 값(앞뒤 공백 제거됨)을 돌려준다. 없으면 NULL. */
const char *http_request_header(const http_request *req, const char *name);

/* D-51 표 2~5행: 1.1은 close 토큰이 없으면 1, 1.0은 keep-alive 토큰이 있고 close가 없으면 1. 그 밖에는 0. */
int http_request_keep_alive(const http_request *req);

#endif /* HTTP_PARSER_H */
