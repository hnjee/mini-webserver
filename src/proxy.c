#define _POSIX_C_SOURCE 200809L

/* D-64·D-65: 백엔드 요청 머리, 응답 머리 검증(판정 순서 1~7), 응답 머리 다시 쓰기. */
#include "proxy.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* D-64: 요청·응답 공통 홉 머리(RFC 9110 7.6.1). Connection 토큰이 가리키는 머리는 따로 본다. */
static const char *const hop_headers[] = {"Connection", "Keep-Alive", "Proxy-Connection", "TE", "Upgrade", NULL};

static int is_tchar(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int is_ows(char c)
{
    return c == ' ' || c == '\t';
}

/* 상태 줄 이유 문구·머리 값에 허용하는 바이트: HTAB, SP, VCHAR, 0x80~0xFF(D-65 3·4단계). */
static int is_field_byte(unsigned char c)
{
    return c == '\t' || c >= 0x20 ? c != 0x7f : 0;
}

/* 이름 s[0..n)이 word와 대소문자 무시로 같은가. */
static int name_is(const char *s, size_t n, const char *word)
{
    return n == strlen(word) && strncasecmp(s, word, n) == 0;
}

static int is_hop(const char *s, size_t n)
{
    size_t i;
    for (i = 0; hop_headers[i] != NULL; i++) {
        if (name_is(s, n, hop_headers[i])) {
            return 1;
        }
    }
    return 0;
}

/* 쉼표 목록 v[0..vn)에 토큰 name[0..nn)이 있는가(앞뒤 공백 제거, 대소문자 무시). */
static int list_has(const char *v, size_t vn, const char *name, size_t nn)
{
    size_t p = 0;
    while (p < vn) {
        size_t start, end;
        while (p < vn && is_ows(v[p])) {
            p++;
        }
        start = p;
        while (p < vn && v[p] != ',') {
            p++;
        }
        end = p;
        while (end > start && is_ows(v[end - 1])) {
            end--;
        }
        if (end - start == nn && strncasecmp(v + start, name, nn) == 0) {
            return 1;
        }
        p++; /* 쉼표 건너뜀 */
    }
    return 0;
}

/* ---- 응답 머리의 줄 순회 ---- */

typedef struct {
    const char *name, *value; /* 값은 앞뒤 공백 제거 */
    size_t name_len, value_len;
    const char *line; /* 줄 시작, line_len은 CRLF 제외 */
    size_t line_len;
} field;

/* 검증된 머리 head[0..head_len)의 [*pos, ...)에서 다음 줄을 읽는다. 빈 줄(머리 끝)이면 0. 모든 CR 뒤는 LF다. */
static int next_field(const char *head, size_t head_len, size_t *pos, field *f)
{
    const char *line = head + *pos, *eol = memchr(line, '\r', head_len - *pos), *colon, *v, *e;

    if (eol == line) {
        return 0;
    }
    colon = memchr(line, ':', (size_t)(eol - line));
    v = colon + 1;
    e = eol;
    while (v < e && is_ows(*v)) {
        v++;
    }
    while (e > v && is_ows(e[-1])) {
        e--;
    }
    f->line = line;
    f->line_len = (size_t)(eol - line);
    f->name = line;
    f->name_len = (size_t)(colon - line);
    f->value = v;
    f->value_len = (size_t)(e - v);
    *pos = (size_t)(eol + 2 - head);
    return 1;
}

/* 응답 머리의 어떤 Connection 줄이 name을 가리키는가. first는 첫 머리 줄 위치. */
static int response_names(const char *head, size_t head_len, size_t first, const char *name, size_t nn)
{
    size_t pos = first;
    field f;
    while (next_field(head, head_len, &pos, &f)) {
        if (name_is(f.name, f.name_len, "Connection") && list_has(f.value, f.value_len, name, nn)) {
            return 1;
        }
    }
    return 0;
}

/* D-65 2단계: NUL 없음, 모든 LF 앞은 CR, CR 뒤는 LF. 범위 마지막 CR은 다음 바이트를 기다린다. */
static int line_breaks_ok(const char *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (d[i] == '\0' || (d[i] == '\n' && (i == 0 || d[i - 1] != '\r')) ||
            (d[i] == '\r' && i + 1 < n && d[i + 1] != '\n')) {
            return 0;
        }
    }
    return 1;
}

/* D-65 3단계. line[0..n)은 CRLF를 뺀 상태 줄. 상태 코드를 돌려주고 실패면 0. */
static int parse_status_line(const char *line, size_t n)
{
    int status;
    size_t i;

    if (n < 12 || memcmp(line, "HTTP/1.", 7) != 0 || (line[7] != '0' && line[7] != '1') || line[8] != ' ') {
        return 0;
    }
    for (i = 9; i < 12; i++) {
        if (line[i] < '0' || line[i] > '9') {
            return 0;
        }
    }
    status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (status < 100 || status > 599 || (n > 12 && line[12] != ' ')) {
        return 0;
    }
    for (i = 13; i < n; i++) {
        if (!is_field_byte((unsigned char)line[i])) {
            return 0;
        }
    }
    return status;
}

/* D-65 4단계 한 줄: token 이름 바로 뒤 ':', 허용 바이트 값. 공백으로 시작하는 줄(obs-fold)은 실패. */
static int field_line_ok(const char *line, size_t n)
{
    const char *colon = memchr(line, ':', n);
    size_t i;

    if (colon == NULL || colon == line) {
        return 0;
    }
    for (i = 0; line + i < colon; i++) {
        if (!is_tchar((unsigned char)line[i])) {
            return 0;
        }
    }
    for (i = (size_t)(colon - line) + 1; i < n; i++) {
        if (!is_field_byte((unsigned char)line[i])) {
            return 0;
        }
    }
    return 1;
}

proxy_parse_result proxy_parse_response(const char *data, size_t len, proxy_response *out)
{
    size_t scope = len < PROXY_RESPONSE_HEAD_MAX ? len : PROXY_RESPONSE_HEAD_MAX, head_len = 0, i, pos, count = 0, cl_count = 0;
    const char *eol;
    size_t cl = 0;
    int status;

    /* 1단계: 길이 기준으로 첫 CRLF CRLF를 8KB 안에서 찾는다 */
    for (i = 3; i < scope && head_len == 0; i++) {
        if (data[i - 3] == '\r' && data[i - 2] == '\n' && data[i - 1] == '\r' && data[i] == '\n') {
            head_len = i + 1;
        }
    }
    /* 2단계는 미완성 머리에도 바로 적용한다: 어느 쪽이든 결과는 502다 */
    if (!line_breaks_ok(data, head_len > 0 ? head_len : scope)) {
        return PROXY_PARSE_BAD;
    }
    if (head_len == 0) {
        return len < PROXY_RESPONSE_HEAD_MAX ? PROXY_PARSE_INCOMPLETE : PROXY_PARSE_BAD;
    }
    /* 3단계 */
    eol = memchr(data, '\r', head_len);
    status = parse_status_line(data, (size_t)(eol - data));
    if (status == 0) {
        return PROXY_PARSE_BAD;
    }
    /* 4단계와 6단계의 값 수집 */
    for (pos = (size_t)(eol - data) + 2; pos < head_len - 2; pos = (size_t)(eol - data) + 2) {
        const char *line = data + pos;
        eol = memchr(line, '\r', head_len - pos);
        if (!field_line_ok(line, (size_t)(eol - line)) || ++count > HTTP_MAX_HEADERS) {
            return PROXY_PARSE_BAD;
        }
    }
    if (status < 200) { /* 5단계 */
        return PROXY_PARSE_BAD;
    }
    /* 6단계: 프레이밍(상태와 관계없이) */
    pos = (size_t)((const char *)memchr(data, '\r', head_len) - data) + 2;
    for (field f; next_field(data, head_len, &pos, &f);) {
        if (name_is(f.name, f.name_len, "Transfer-Encoding") ||
            (name_is(f.name, f.name_len, "Connection") && (list_has(f.value, f.value_len, "Content-Length", 14) ||
                                                           list_has(f.value, f.value_len, "Transfer-Encoding", 17)))) {
            return PROXY_PARSE_BAD;
        }
        if (name_is(f.name, f.name_len, "Content-Length")) {
            size_t i;
            if (++cl_count > 1 || f.value_len == 0) {
                return PROXY_PARSE_BAD;
            }
            for (i = 0; i < f.value_len; i++) {
                size_t d = (size_t)(f.value[i] - '0');
                if (f.value[i] < '0' || f.value[i] > '9' || cl > (SIZE_MAX - d) / 10) {
                    return PROXY_PARSE_BAD;
                }
                cl = cl * 10 + d;
            }
        }
    }
    /* 7단계 */
    out->head_len = head_len;
    out->status = status;
    out->content_length = cl;
    out->body = status == 204 || status == 304 ? PROXY_BODY_NONE : cl_count == 1 ? PROXY_BODY_LENGTH : PROXY_BODY_EOF;
    return PROXY_PARSE_OK;
}

/* ---- 머리 쓰기 ---- */

typedef struct {
    char *buf;
    size_t cap, len;
    int overflow;
} writer;

static void put(writer *w, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (w->overflow) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->overflow = 1;
        return;
    }
    w->len += (size_t)n;
}

static int done(writer *w)
{
    if (w->overflow) {
        errno = EINVAL;
        return -1;
    }
    return (int)w->len;
}

/* 요청 머리 name이 Connection 토큰으로 지명됐는가. */
static int request_names(const http_request *req, const char *name)
{
    size_t i;
    for (i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, "Connection") == 0 &&
            list_has(req->headers[i].value, strlen(req->headers[i].value), name, strlen(name))) {
            return 1;
        }
    }
    return 0;
}

int proxy_format_request(char *buf, size_t cap, const http_request *req, const char *target, const char *authority)
{
    writer w = {buf, cap, 0, 0};
    size_t i;

    if (cap == 0) {
        errno = EINVAL;
        return -1;
    }
    put(&w, "%s %s HTTP/1.0\r\n", req->method, target);
    for (i = 0; i < req->header_count; i++) {
        const char *name = req->headers[i].name;
        size_t n = strlen(name);
        if (is_hop(name, n) || name_is(name, n, "Host") || name_is(name, n, "Expect") ||
            name_is(name, n, "Content-Length") || request_names(req, name)) {
            continue;
        }
        put(&w, "%s: %s\r\n", name, req->headers[i].value);
    }
    put(&w, "Host: %s\r\nConnection: close\r\n", authority);
    if (http_request_header(req, "Content-Length") != NULL) {
        put(&w, "Content-Length: %zu\r\n", req->content_length);
    }
    put(&w, "\r\n");
    return done(&w);
}

int proxy_format_response_head(char *buf, size_t cap, const char *head, const proxy_response *resp,
                               http_connection connection)
{
    writer w = {buf, cap, 0, 0};
    const char *eol = memchr(head, '\r', resp->head_len);
    size_t pos = (size_t)(eol - head) + 2, first = pos;
    field f;

    if (cap == 0) {
        errno = EINVAL;
        return -1;
    }
    put(&w, "HTTP/1.1%.*s\r\n", (int)(eol - head - 8), head + 8);
    while (next_field(head, resp->head_len, &pos, &f)) {
        if (is_hop(f.name, f.name_len) || response_names(head, resp->head_len, first, f.name, f.name_len) ||
            (resp->status == 204 && name_is(f.name, f.name_len, "Content-Length"))) {
            continue;
        }
        put(&w, "%.*s\r\n", (int)f.line_len, f.line);
    }
    put(&w, "%s\r\n", connection == HTTP_CONNECTION_CLOSE        ? "Connection: close\r\n"
                      : connection == HTTP_CONNECTION_KEEP_ALIVE ? "Connection: keep-alive\r\n"
                                                                 : "");
    return done(&w);
}
