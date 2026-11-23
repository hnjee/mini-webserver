#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_MAX_HEADERS 32

/* 헤더 한 줄. name, value는 헤더 버퍼 안을 가리켜요 */
struct http_header {
    char *name;
    char *value;
};

/* 파싱 결과. 모든 포인터는 헤더 버퍼 안을 가리켜요 */
struct http_request {
    char *method;
    char *path;
    char *version;
    struct http_header headers[HTTP_MAX_HEADERS];
    int header_count;
};

/* buf[0..len)에서 헤더의 끝(\r\n\r\n)을 찾아요. 있으면 끝 바로 다음 위치, 없으면 0 */
size_t http_find_header_end(const char *buf, size_t len);

/* 헤더의 끝까지 든 buf를 파싱해 req를 채워요. buf 안에 '\0'을 써요. 성공 0, 실패하면 응답할 상태 코드 */
int http_parse_request(char *buf, size_t len, struct http_request *req);

/* 헤더 이름을 대소문자 구분 없이 찾아 값을 돌려줘요. 없으면 NULL */
const char *http_find_header(const struct http_request *req, const char *name);

/* 상태 코드의 문구 (예: 501 → "Not Implemented") */
const char *http_reason(int status);

#endif
