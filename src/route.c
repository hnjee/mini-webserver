#define _POSIX_C_SOURCE 200809L

/* D-61~D-63: 라우팅 설정 파일 → 표, 가장 긴 접두 매칭, 규칙별 라운드 로빈. */
#include "route.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fail(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error, size, fmt, ap);
    va_end(ap);
    return -1;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* 제어 문자(0x00~0x1F, 0x7F). 공백·탭은 이미 구분자로 쓰였다. */
static int has_control(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) {
            return 1;
        }
    }
    return 0;
}

/* "호스트:포트" 하나를 검사해 b에 채운다. 마지막 ':' 뒤가 포트다. */
static int parse_backend(const char *tok, size_t n, route_backend *b, int line, char *error, size_t size)
{
    const char *colon = NULL;
    size_t i, host_len;
    long port = 0;

    for (i = 0; i < n; i++) {
        if (tok[i] == ':') {
            colon = tok + i;
        }
    }
    if (colon == NULL || colon == tok || colon + 1 == tok + n) {
        return fail(error, size, "%d행: 백엔드는 호스트:포트 형식이어야 한다", line);
    }
    if (n >= sizeof(b->authority)) { /* F1: 포트 앞자리 0이 많아도 authority 복사가 넘치지 않게 토큰 길이를 먼저 막는다 */
        return fail(error, size, "%d행: 백엔드 호스트:포트가 %zu바이트를 넘는다", line, sizeof(b->authority) - 1);
    }
    host_len = (size_t)(colon - tok);
    if (host_len > ROUTE_HOST_MAX) {
        return fail(error, size, "%d행: 호스트가 %d바이트를 넘는다", line, ROUTE_HOST_MAX);
    }
    for (i = host_len + 1; i < n; i++) {
        if (tok[i] < '0' || tok[i] > '9' || port > 65535) {
            return fail(error, size, "%d행: 포트는 1~65535 숫자여야 한다", line);
        }
        port = port * 10 + (tok[i] - '0');
    }
    if (port < 1 || port > 65535) {
        return fail(error, size, "%d행: 포트는 1~65535 숫자여야 한다", line);
    }
    memcpy(b->host, tok, host_len);
    b->host[host_len] = '\0';
    memcpy(b->authority, tok, n);
    b->authority[n] = '\0';
    b->port = (int)port;
    memset(&b->addr, 0, sizeof(b->addr));
    return 0;
}

/* 한 줄(주석·줄 끝 제거됨)을 해석한다. 빈 줄은 0이고 규칙을 더하지 않는다. */
static int parse_line(const char *s, size_t n, int line, route_table *out, char *error, size_t size)
{
    const char *tok[2 + ROUTE_MAX_BACKENDS];
    size_t len[2 + ROUTE_MAX_BACKENDS], count = 0, i, p = 0;
    route_rule *r;

    while (p < n) {
        size_t start;
        while (p < n && is_space(s[p])) {
            p++;
        }
        if (p == n) {
            break;
        }
        start = p;
        while (p < n && !is_space(s[p])) {
            p++;
        }
        if (count == sizeof(tok) / sizeof(tok[0])) {
            return fail(error, size, "%d행: 백엔드는 %d개까지다", line, ROUTE_MAX_BACKENDS);
        }
        tok[count] = s + start;
        len[count++] = p - start;
        if (has_control(s + start, p - start)) {
            return fail(error, size, "%d행: 제어 문자가 있다", line);
        }
    }
    if (count == 0) {
        return 0;
    }
    if (len[0] != 5 || memcmp(tok[0], "route", 5) != 0) {
        return fail(error, size, "%d행: route 줄만 쓸 수 있다", line);
    }
    if (count < 3) {
        return fail(error, size, "%d행: route <접두 경로> <호스트:포트>... 형식이어야 한다", line);
    }
    if (len[1] > ROUTE_PREFIX_MAX || tok[1][0] != '/' || tok[1][len[1] - 1] != '/' || memchr(tok[1], '?', len[1]) != NULL) {
        return fail(error, size, "%d행: 접두 경로는 /로 시작하고 /로 끝나며 ?를 넣을 수 없다", line);
    }
    for (i = 0; i < out->count; i++) {
        if (out->rules[i].prefix_len == len[1] && memcmp(out->rules[i].prefix, tok[1], len[1]) == 0) {
            return fail(error, size, "%d행: 접두 경로가 %d행과 겹친다", line, out->rules[i].line);
        }
    }
    if (out->count == ROUTE_MAX_RULES) {
        return fail(error, size, "%d행: 규칙은 %d개까지다", line, ROUTE_MAX_RULES);
    }
    r = &out->rules[out->count];
    memcpy(r->prefix, tok[1], len[1]);
    r->prefix[len[1]] = '\0';
    r->prefix_len = len[1];
    r->backend_count = 0;
    r->next = 0;
    r->line = line;
    for (i = 2; i < count; i++) {
        if (parse_backend(tok[i], len[i], &r->backends[r->backend_count], line, error, size) != 0) {
            return -1;
        }
        r->backend_count++;
    }
    out->count++;
    return 0;
}

int route_parse(const char *text, size_t len, route_table *out, char *error, size_t error_size)
{
    size_t p = 0;
    int line = 0;

    out->count = 0;
    while (p < len) {
        const char *nl = memchr(text + p, '\n', len - p);
        size_t end = nl != NULL ? (size_t)(nl - text) : len, n;
        const char *hash = memchr(text + p, '#', end - p);

        line++;
        n = (hash != NULL ? (size_t)(hash - text) : end) - p;
        if (hash == NULL && n > 0 && text[p + n - 1] == '\r') {
            n--; /* CRLF 줄 끝 */
        }
        if (parse_line(text + p, n, line, out, error, error_size) != 0) {
            return -1;
        }
        p = end + 1;
    }
    if (out->count == 0) {
        return fail(error, error_size, "설정에 route 규칙이 하나도 없다");
    }
    return 0;
}

int route_resolve(route_table *table, char *error, size_t error_size)
{
    size_t i, j;

    for (i = 0; i < table->count; i++) {
        for (j = 0; j < table->rules[i].backend_count; j++) {
            route_backend *b = &table->rules[i].backends[j];
            struct addrinfo hints, *res = NULL;
            int rc;

            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET; /* D-61: IPv4만 */
            hints.ai_socktype = SOCK_STREAM;
            rc = getaddrinfo(b->host, NULL, &hints, &res);
            if (rc != 0 || res == NULL) {
                return fail(error, error_size, "%d행: 이름 조회 실패 %s (%s)", table->rules[i].line, b->host,
                            gai_strerror(rc));
            }
            memcpy(&b->addr, res->ai_addr, sizeof(b->addr));
            b->addr.sin_port = htons((unsigned short)b->port);
            freeaddrinfo(res);
        }
    }
    return 0;
}

int route_load(const char *path, route_table *out, char *error, size_t error_size)
{
    static char text[ROUTE_FILE_MAX + 1];
    size_t len = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return fail(error, error_size, "-c: %s 를 열 수 없다 (%s)", path, strerror(errno));
    }
    for (;;) {
        ssize_t n = read(fd, text + len, sizeof(text) - len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            int saved = errno;
            close(fd);
            return fail(error, error_size, "-c: %s 읽기 실패 (%s)", path, strerror(saved));
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
        if (len > ROUTE_FILE_MAX) {
            close(fd);
            return fail(error, error_size, "-c: %s 가 %d바이트를 넘는다", path, ROUTE_FILE_MAX);
        }
    }
    close(fd);
    if (route_parse(text, len, out, error, error_size) != 0 || route_resolve(out, error, error_size) != 0) {
        return -1;
    }
    return 0;
}

route_rule *route_match(route_table *table, const char *target)
{
    route_rule *best = NULL;
    size_t i;

    for (i = 0; table != NULL && i < table->count; i++) {
        route_rule *r = &table->rules[i];
        if (strncmp(target, r->prefix, r->prefix_len) == 0 && (best == NULL || r->prefix_len > best->prefix_len)) {
            best = r;
        }
    }
    return best;
}

const char *route_forward_target(const route_rule *rule, const char *target)
{
    return target + rule->prefix_len - 1; /* 접두의 마지막 '/'부터 */
}

const route_backend *route_pick(route_rule *rule)
{
    return &rule->backends[rule->next++ % rule->backend_count];
}
