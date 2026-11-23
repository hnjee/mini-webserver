#include "request.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

ssize_t request_find_end(const char *buf, size_t len)
{
    /* buf는 '\0'으로 끝나지 않아서 strstr()을 쓸 수 없고, memmem()은 POSIX가 아니다 */
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return (ssize_t)(i + 4);
    }
    return -1;
}

/*
 * *pos부터 "\r\n" 앞까지를 한 줄로 잘라 돌려준다. '\r' 자리에 '\0'을 쓰고,
 * *pos는 다음 줄의 시작으로 옮긴다. 줄 끝이 없거나 줄 안에 '\0'이나
 * '\n'만 홀로 있으면 NULL을 돌려준다 (형식이 틀린 요청).
 */
static char *next_line(char **pos, char *end)
{
    char *start = *pos;

    for (char *p = start; p + 1 < end; p++) {
        if (p[0] == '\r' && p[1] == '\n') {
            *p = '\0';
            *pos = p + 2;
            return start;
        }
        if (*p == '\0' || *p == '\n')
            return NULL;
    }
    return NULL;
}

/* 앞뒤 공백과 탭을 잘라낸 문자열을 돌려준다 (제자리에서 '\0'을 쓴다) */
static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    *e = '\0';
    return s;
}

/* "123"처럼 숫자로만 된 0 이상의 정수인지 검사하고 바꾼다. 실패하면 -1 */
static long parse_content_length(const char *s)
{
    char *endp;
    long v;

    if (*s == '\0')
        return -1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')   /* 부호, 공백, 글자를 모두 거른다 */
            return -1;
    }
    errno = 0;
    v = strtol(s, &endp, 10);
    if (errno == ERANGE || *endp != '\0')
        return -1;
    return v;
}

/* 요청 라인 "메서드 URI 버전"을 공백 하나씩으로 나눈다. 정상이면 0, 아니면 400 */
static int parse_request_line(char *line, struct http_request *req)
{
    char *sp1, *sp2;

    sp1 = strchr(line, ' ');
    if (sp1 == NULL)
        return 400;
    sp2 = strchr(sp1 + 1, ' ');
    if (sp2 == NULL || strchr(sp2 + 1, ' ') != NULL)
        return 400;                         /* 세 조각이 아니다 */
    *sp1 = '\0';
    *sp2 = '\0';

    if (line[0] == '\0' || sp1[1] == '\0' || sp2[1] == '\0')
        return 400;                         /* 빈 조각 ("GET  / HTTP/1.1") */
    if (sp1[1] != '/')
        return 400;                         /* URI는 '/'로 시작해야 한다 */
    if (strncmp(sp2 + 1, "HTTP/", 5) != 0)
        return 400;                         /* 버전 형식이 아니다 */

    req->method = line;
    req->uri = sp1 + 1;
    req->version = sp2 + 1;
    return 0;
}

/* 헤더 한 줄 "이름: 값"을 읽어 필요한 것만 req에 담는다. 정상이면 0, 아니면 400 */
static int parse_header_line(char *line, struct http_request *req)
{
    char *colon = strchr(line, ':');
    char *name, *value;

    if (colon == NULL || colon == line)
        return 400;                         /* ':'가 없거나 이름이 비었다 */
    *colon = '\0';
    name = line;
    if (strpbrk(name, " \t") != NULL)
        return 400;                         /* "Host : x"처럼 이름에 공백 */
    value = trim(colon + 1);

    /* 헤더 이름은 대소문자를 구분하지 않는다 */
    if (strcasecmp(name, "Host") == 0) {
        if (req->host != NULL)
            return 400;                     /* Host가 두 번 (RFC 9112) */
        req->host = value;
    } else if (strcasecmp(name, "Content-Length") == 0) {
        long v = parse_content_length(value);
        if (v < 0 || req->content_length >= 0)
            return 400;                     /* 숫자가 아니거나 두 번 */
        req->content_length = v;
    } else if (strcasecmp(name, "Connection") == 0) {
        req->connection = value;
    }
    /* 그 밖의 헤더는 형식만 검사하고 버린다 */
    return 0;
}

int parse_request(char *buf, size_t len, struct http_request *req)
{
    char *pos = buf;
    char *end = buf + len;
    char *line;
    int status;

    req->method = NULL;
    req->uri = NULL;
    req->version = NULL;
    req->host = NULL;
    req->content_length = -1;
    req->connection = NULL;

    /* 1. 요청 라인 */
    line = next_line(&pos, end);
    if (line == NULL)
        return 400;
    status = parse_request_line(line, req);
    if (status != 0)
        return status;

    /* 2. 헤더 줄들: 빈 줄이 나오면 끝 */
    for (;;) {
        line = next_line(&pos, end);
        if (line == NULL)
            return 400;
        if (line[0] == '\0')
            break;
        status = parse_header_line(line, req);
        if (status != 0)
            return status;
    }

    /* 3. 형식은 맞다. 메서드(501) → 버전(505) 순서로 판정한다 */
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0)
        return 501;
    if (strcmp(req->version, "HTTP/1.1") != 0 && strcmp(req->version, "HTTP/1.0") != 0)
        return 505;

    /* 4. HTTP/1.1은 Host가 필수다 */
    if (strcmp(req->version, "HTTP/1.1") == 0 && req->host == NULL)
        return 400;

    return 0;
}
