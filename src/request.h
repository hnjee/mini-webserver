#ifndef REQUEST_H
#define REQUEST_H

#include <stddef.h>
#include <sys/types.h>

/* 연결마다 쓰는 헤더 버퍼 크기. 넘치면 431로 응답한다 */
#define REQUEST_BUF_SIZE 8192

/*
 * 파싱한 요청. 문자열 필드는 모두 parse_request()에 넘긴 버퍼 안을 가리킨다.
 * 그래서 요청 처리가 끝나기 전에는 그 버퍼를 다시 채우거나 옮기면 안 된다.
 */
struct http_request {
    char *method;          /* "GET" */
    char *uri;             /* "/index.html" */
    char *version;         /* "HTTP/1.1" */
    char *host;            /* 없으면 NULL */
    long  content_length;  /* 없으면 -1 */
    char *connection;      /* 없으면 NULL */
};

/*
 * buf의 len바이트에서 헤더 끝 "\r\n\r\n"을 찾는다.
 * 찾으면 그 바로 뒤 위치(= 헤더 길이)를, 없으면 -1을 돌려준다.
 */
ssize_t request_find_end(const char *buf, size_t len);

/*
 * buf의 앞 len바이트(헤더, "\r\n\r\n"까지)를 제자리에서 조각내 req를 채운다.
 * 구분자 자리에 '\0'을 써 넣으므로 buf 내용이 바뀐다.
 * 정상이면 0, 아니면 응답할 상태 코드(400, 501, 505)를 돌려준다.
 */
int parse_request(char *buf, size_t len, struct http_request *req);

#endif
