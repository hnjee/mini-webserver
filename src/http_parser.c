#define _POSIX_C_SOURCE 200809L

#include "http_parser.h"

#include <string.h>
#include <strings.h>

/* RFC 9110 tchar */
static int is_tchar(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int is_ows(char c)
{
    return c == ' ' || c == '\t';
}

/* data[0..len)에서 첫 \r\n\r\n을 길이 기준으로 찾아 그 끝 위치를 돌려준다. 없으면 0. NUL이 있어도 멈추지 않는다. */
static size_t find_head_end(const char *data, size_t len)
{
    size_t i;
    for (i = 3; i < len; i++) {
        if (data[i - 3] == '\r' && data[i - 2] == '\n' && data[i - 1] == '\r' && data[i] == '\n') {
            return i + 1;
        }
    }
    return 0;
}

/* D-15 1행: NUL, 뒤에 \n 없는 \r, \r 없는 \n이 있으면 0. 범위의 마지막 \r은 다음 바이트를 기다린다. */
static int line_breaks_ok(const char *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\0') {
            return 0;
        }
        if (c == '\n' && (i == 0 || data[i - 1] != '\r')) {
            return 0;
        }
        if (c == '\r' && i + 1 < len && data[i + 1] != '\n') {
            return 0;
        }
    }
    return 1;
}

/* src[0..n)을 *cursor 위치에 NUL로 끝나게 복사하고 복사본을 돌려준다. */
static const char *copy_out(char **cursor, const char *src, size_t n)
{
    char *dst = *cursor;
    memcpy(dst, src, n);
    dst[n] = '\0';
    *cursor = dst + n + 1;
    return dst;
}

/* line[0..len)은 \r\n을 뺀 요청 줄. D-15 2~5행을 검사하고, 통과하면 out에 복사한다. */
static http_parse_result parse_request_line(const char *line, size_t len, http_request *out,
                                            char **cursor)
{
    const char *sp1 = memchr(line, ' ', len);
    const char *target, *sp2, *version;
    size_t method_len, target_len, version_len, i;

    if (sp1 == NULL) {
        return HTTP_PARSE_BAD_REQUEST;
    }
    method_len = (size_t)(sp1 - line);
    target = sp1 + 1;
    sp2 = memchr(target, ' ', len - method_len - 1);
    if (sp2 == NULL) {
        return HTTP_PARSE_BAD_REQUEST;
    }
    target_len = (size_t)(sp2 - target);
    version = sp2 + 1;
    version_len = len - (size_t)(version - line);
    if (method_len == 0 || target_len == 0 || version_len == 0) {
        return HTTP_PARSE_BAD_REQUEST;
    }
    for (i = 0; i < method_len; i++) {
        if (!is_tchar((unsigned char)line[i])) {
            return HTTP_PARSE_BAD_REQUEST;
        }
    }
    for (i = 0; i < target_len; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c <= ' ' || c == 0x7f) {
            return HTTP_PARSE_BAD_REQUEST;
        }
    }
    /* 3행: 정확히 HTTP/숫자.숫자. 버전 뒤 공백(세 번째 공백)도 길이가 달라 여기서 400이 된다 */
    if (version_len != 8 || memcmp(version, "HTTP/", 5) != 0 || version[5] < '0' ||
        version[5] > '9' || version[6] != '.' || version[7] < '0' || version[7] > '9') {
        return HTTP_PARSE_BAD_REQUEST;
    }
    /* 4행 */
    if (version[5] != '1' || (version[7] != '0' && version[7] != '1')) {
        return HTTP_PARSE_VERSION_NOT_SUPPORTED;
    }
    /* 5행 */
    if (!(method_len == 3 && memcmp(line, "GET", 3) == 0) &&
        !(method_len == 4 && memcmp(line, "POST", 4) == 0)) {
        return HTTP_PARSE_NOT_IMPLEMENTED;
    }
    out->method = copy_out(cursor, line, method_len);
    out->target = copy_out(cursor, target, target_len);
    out->version = copy_out(cursor, version, version_len);
    out->version_minor = version[7] - '0';
    return HTTP_PARSE_OK;
}

/* line[0..len)은 \r\n을 뺀 헤더 줄. D-15 6~7행을 검사하고, 통과하면 out에 덧붙인다. */
static http_parse_result parse_header_line(const char *line, size_t len, http_request *out,
                                           char **cursor)
{
    const char *colon = memchr(line, ':', len);
    const char *value, *end;
    size_t name_len, i;

    if (colon == NULL || colon == line) {
        return HTTP_PARSE_BAD_REQUEST; /* 콜론 없음, 빈 이름 */
    }
    name_len = (size_t)(colon - line);
    for (i = 0; i < name_len; i++) {
        if (!is_tchar((unsigned char)line[i])) {
            return HTTP_PARSE_BAD_REQUEST; /* 이름 뒤 공백, 첫 글자 공백·탭(obs-fold) 포함 */
        }
    }
    /* 7행: 101번째 헤더는 배열에 쓰기 전에 거절한다(431, D-42) */
    if (out->header_count == HTTP_MAX_HEADERS) {
        return HTTP_PARSE_HEADER_TOO_LARGE;
    }
    value = colon + 1;
    end = line + len;
    while (value < end && is_ows(*value)) {
        value++;
    }
    while (end > value && is_ows(end[-1])) {
        end--;
    }
    out->headers[out->header_count].name = copy_out(cursor, line, name_len);
    out->headers[out->header_count].value = copy_out(cursor, value, (size_t)(end - value));
    out->header_count++;
    return HTTP_PARSE_OK;
}

/*
 * head[0..len)은 \r\n\r\n으로 끝나고 줄 바꿈 검사를 통과한 머리(모든 \r 뒤에 \n). D-15 2~9행.
 * 줄마다 구분자(공백·콜론·\r\n) 자리를 NUL로 쓰므로 복사본 합계는 len보다 작아 storage가 넘치지 않는다.
 */
static http_parse_result parse_head(const char *head, size_t len, http_request *out)
{
    char *cursor = out->storage;
    const char *end = head + len - 2; /* 종료 빈 줄의 \r */
    const char *line = head;
    const char *eol = memchr(line, '\r', (size_t)(end - line));
    http_parse_result r;
    size_t i, host_count = 0;

    out->header_count = 0;
    r = parse_request_line(line, (size_t)(eol - line), out, &cursor);
    if (r != HTTP_PARSE_OK) {
        return r;
    }
    for (line = eol + 2; line < end; line = eol + 2) {
        eol = memchr(line, '\r', (size_t)(end - line));
        r = parse_header_line(line, (size_t)(eol - line), out, &cursor);
        if (r != HTTP_PARSE_OK) {
            return r;
        }
    }
    /* 8~9행 */
    for (i = 0; i < out->header_count; i++) {
        if (strcasecmp(out->headers[i].name, "Host") == 0) {
            host_count++;
        }
    }
    if (host_count > 1 || (out->version_minor == 1 && host_count == 0)) {
        return HTTP_PARSE_BAD_REQUEST;
    }
    return HTTP_PARSE_OK;
}

/* D-48 순서 1: 첫 SP 다음부터 SP·\r·\n·NUL 또는 data 끝 전까지가 대상이다. 첫 SP 앞에 줄 끝이 오면 대상이 없다. */
static int target_too_long(const char *data, size_t len)
{
    size_t i = 0, start;
    while (i < len && data[i] != ' ' && data[i] != '\r' && data[i] != '\n' && data[i] != '\0') {
        i++;
    }
    if (i == len || data[i] != ' ') {
        return 0;
    }
    start = ++i;
    while (i < len && i - start <= HTTP_REQUEST_TARGET_MAX && data[i] != ' ' && data[i] != '\r' &&
           data[i] != '\n' && data[i] != '\0') {
        i++;
    }
    return i - start > HTTP_REQUEST_TARGET_MAX;
}

/* *s에서 다음 쉼표 요소를 앞뒤 공백 없이 찾는다. 빈 요소는 건너뛴다. 없으면 0. */
static int next_token(const char **s, const char **tok, size_t *tok_len)
{
    const char *p = *s;
    for (;;) {
        const char *start, *end;
        while (is_ows(*p)) {
            p++;
        }
        if (*p == '\0') {
            return 0;
        }
        start = p;
        while (*p != ',' && *p != '\0') {
            p++;
        }
        end = p;
        while (end > start && is_ows(end[-1])) {
            end--;
        }
        if (*p == ',') {
            p++;
        }
        if (end > start) {
            *tok = start;
            *tok_len = (size_t)(end - start);
            *s = p;
            return 1;
        }
    }
}

static int token_is(const char *tok, size_t len, const char *word)
{
    return len == strlen(word) && strncasecmp(tok, word, len) == 0;
}

/* D-48 순서 5~7: Transfer-Encoding, Content-Length 형식(D-49), 1MB 초과. 통과하면 content_length를 채운다. */
static http_parse_result parse_body_length(http_request *out)
{
    const char *cl = NULL, *last = NULL, *p, *tok;
    size_t i, cl_count = 0, last_len = 0, tok_len, value = 0;
    int has_te = 0, too_large = 0;

    for (i = 0; i < out->header_count; i++) {
        if (strcasecmp(out->headers[i].name, "Transfer-Encoding") == 0) {
            has_te = 1;
            for (p = out->headers[i].value; next_token(&p, &tok, &tok_len);) {
                last = tok;
                last_len = tok_len;
            }
        } else if (strcasecmp(out->headers[i].name, "Content-Length") == 0) {
            cl_count++;
            cl = out->headers[i].value;
        }
    }
    if (has_te) {
        return last != NULL && token_is(last, last_len, "chunked") ? HTTP_PARSE_NOT_IMPLEMENTED : HTTP_PARSE_BAD_REQUEST;
    }
    out->content_length = 0;
    if (cl_count == 0) {
        return HTTP_PARSE_OK;
    }
    if (cl_count > 1 || *cl == '\0') {
        return HTTP_PARSE_BAD_REQUEST;
    }
    /* 형식(순서 6)을 끝까지 본 뒤 크기(순서 7)를 본다. 넘는 순간 더 곱하지 않아 자릿수가 길어도 넘치지 않는다. */
    for (p = cl; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return HTTP_PARSE_BAD_REQUEST;
        }
        if (!too_large) {
            value = value * 10 + (size_t)(*p - '0');
            too_large = value > HTTP_CONTENT_LENGTH_MAX;
        }
    }
    if (too_large) {
        return HTTP_PARSE_CONTENT_TOO_LARGE;
    }
    out->content_length = value;
    return HTTP_PARSE_OK;
}

http_parse_result http_parse_request(const char *data, size_t len, http_request *out)
{
    size_t scope = len < HTTP_REQUEST_HEAD_MAX ? len : HTTP_REQUEST_HEAD_MAX;
    size_t head_len = find_head_end(data, scope);
    size_t seen = head_len > 0 ? head_len : scope;
    http_parse_result r;

    if (target_too_long(data, seen)) {
        return HTTP_PARSE_URI_TOO_LONG;
    }
    if (!line_breaks_ok(data, seen)) {
        return HTTP_PARSE_BAD_REQUEST;
    }
    if (head_len == 0) {
        return len < HTTP_REQUEST_HEAD_MAX ? HTTP_PARSE_INCOMPLETE : HTTP_PARSE_HEADER_TOO_LARGE;
    }
    r = parse_head(data, head_len, out);
    if (r != HTTP_PARSE_OK) {
        return r;
    }
    out->head_len = head_len;
    return parse_body_length(out);
}

const char *http_request_header(const http_request *req, const char *name)
{
    size_t i;
    for (i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

int http_request_keep_alive(const http_request *req)
{
    const char *p, *tok;
    size_t i, tok_len;
    int close_token = 0, keep_token = 0;

    for (i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, "Connection") != 0) {
            continue;
        }
        for (p = req->headers[i].value; next_token(&p, &tok, &tok_len);) {
            close_token |= token_is(tok, tok_len, "close");
            keep_token |= token_is(tok, tok_len, "keep-alive");
        }
    }
    if (close_token) {
        return 0;
    }
    return req->version_minor == 1 || keep_token;
}
