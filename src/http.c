/*
 * HTTP 요청 파싱: docs/design/step2.md
 * 소켓을 모르고 헤더 버퍼만 다룬다. 파싱 결과는 버퍼 안에 '\0'을 써서 나누고 포인터로 가리킨다.
 */
#include "http.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* 앞뒤 공백과 탭을 지운 시작 위치를 돌려줘요 */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return s;
}

/* "HTTP/숫자.숫자" 모양인지 */
static int is_version_shape(const char *v)
{
    return strlen(v) == 8 && strncmp(v, "HTTP/", 5) == 0 &&
           isdigit((unsigned char)v[5]) && v[6] == '.' && isdigit((unsigned char)v[7]);
}

size_t http_find_header_end(const char *buf, size_t len)
{
    for (size_t i = 3; i < len; i++) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' &&
            buf[i - 1] == '\r' && buf[i] == '\n')
            return i + 1;
    }
    return 0;
}

/* 요청 줄을 메서드, 경로, 버전으로 나눠요. 성공 0, 실패하면 400·501·505 */
static int parse_request_line(char *line, struct http_request *req)
{
    char *sp1 = strchr(line, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (sp2 == NULL || strchr(sp2 + 1, ' ') != NULL)
        return 400;                              /* 세 부분이 아님 */

    *sp1 = '\0';
    *sp2 = '\0';
    req->method = line;
    req->path = sp1 + 1;
    req->version = sp2 + 1;

    if (req->method[0] == '\0' || req->path[0] != '/' || !is_version_shape(req->version))
        return 400;
    if (strcmp(req->method, "GET") != 0)
        return 501;
    if (strcmp(req->version, "HTTP/1.0") != 0 && strcmp(req->version, "HTTP/1.1") != 0)
        return 505;
    return 0;
}

/* 헤더 줄을 하나씩 첫 번째 :에서 이름과 값으로 나눠요. 성공 0, 형식이 틀리면 400 */
static int parse_headers(char *p, struct http_request *req)
{
    req->header_count = 0;
    while (p != NULL && *p != '\0') {
        char *next = strstr(p, "\r\n");
        if (next != NULL) {
            *next = '\0';
            next += 2;
        }

        char *colon = strchr(p, ':');
        if (colon == NULL || colon == p || colon[-1] == ' ' || colon[-1] == '\t')
            return 400;                          /* : 없음, 이름 비었음, : 앞 공백 */
        *colon = '\0';

        struct http_header *h = &req->headers[req->header_count++];
        h->name = p;
        h->value = trim(colon + 1);
        p = next;
    }
    return 0;
}

int http_parse_request(char *buf, size_t len, struct http_request *req)
{
    if (memchr(buf, '\0', len) != NULL)
        return 400;                              /* 요청 안에 \0 */

    buf[len - 4] = '\0';                         /* 헤더 영역을 C 문자열로 */

    int lines = 1;
    for (char *p = buf; (p = strstr(p, "\r\n")) != NULL; p += 2)
        lines++;
    if (lines - 1 > HTTP_MAX_HEADERS)
        return 431;

    char *rest = strstr(buf, "\r\n");            /* 요청 줄 끝 */
    if (rest != NULL) {
        *rest = '\0';
        rest += 2;
    }

    int status = parse_request_line(buf, req);
    if (status != 0)
        return status;

    status = parse_headers(rest, req);
    if (status != 0)
        return status;

    if (strcmp(req->version, "HTTP/1.1") == 0 && http_find_header(req, "Host") == NULL)
        return 400;
    return 0;
}

const char *http_find_header(const struct http_request *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}
