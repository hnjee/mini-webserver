/*
 * Step 2 파싱 단위 테스트: docs/tests/step2.md 3장 UT-01 ~ UT-39
 * http.c 함수를 소켓 없이 문자열만으로 확인한다.
 * http_parse_request()는 버퍼 안에 '\0'을 쓰므로, 요청을 배열에 복사해서 넘긴다.
 */
#include "http.h"

#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_fail = 0;

#define CHECK(id, cond)                                                   \
    do {                                                                  \
        g_run++;                                                          \
        if (cond) {                                                       \
            printf("  ok    %s\n", id);                                   \
        } else {                                                          \
            g_fail++;                                                     \
            printf("  FAIL  %s  (%s:%d) %s\n", id, __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

/* 문자열 상수와 그 길이 (안에 '\0'이 있어도 끝까지) */
#define S(lit) (lit), (sizeof(lit) - 1)

#define NO_HEADER_END (-100)

static char g_buf[8192];
static struct http_request g_req;

static int str_eq(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* 요청을 g_buf에 복사하고 헤더의 끝까지 파싱한다. 헤더의 끝이 없으면 NO_HEADER_END */
static int parse(const char *req, size_t len)
{
    memset(&g_req, 0, sizeof(g_req));
    memcpy(g_buf, req, len);
    size_t end = http_find_header_end(g_buf, len);
    if (end == 0)
        return NO_HEADER_END;
    return http_parse_request(g_buf, end, &g_req);
}

/* 요청 줄 + Host 헤더 + 헤더 n_extra줄(X-1 ~ X-n)을 out에 만든다. 길이를 돌려준다 */
static size_t build_many_headers(char *out, size_t cap, const char *request_line, int n_extra)
{
    size_t len = (size_t)snprintf(out, cap, "%s\r\nHost: x\r\n", request_line);
    for (int i = 1; i <= n_extra; i++)
        len += (size_t)snprintf(out + len, cap - len, "X-%d: a\r\n", i);
    len += (size_t)snprintf(out + len, cap - len, "\r\n");
    return len;
}

/* ---- http_find_header_end ---- */

static void ut01_to_ut04_header_end(void)
{
    CHECK("UT-01 헤더의 끝이 있으면 끝 다음 위치",
          http_find_header_end(S("GET / HTTP/1.1\r\nHost: x\r\n\r\n")) == 27);
    CHECK("UT-02 헤더의 끝이 없으면 0",
          http_find_header_end(S("GET / HTTP/1.1\r\nHost: x\r\n")) == 0);
    CHECK("UT-03 \\n 하나 모자라면 0",
          http_find_header_end(S("GET / HTTP/1.1\r\nHost: x\r\n\r")) == 0);
    CHECK("UT-04 뒤에 다음 요청이 붙어도 첫 헤더의 끝",
          http_find_header_end(S("GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /next HTTP/1.1\r\n")) == 27);
}

/* ---- 올바른 요청 ---- */

static void ut05_basic(void)
{
    int r = parse(S("GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    CHECK("UT-05 리턴 0", r == 0);
    CHECK("UT-05 method GET", str_eq(g_req.method, "GET"));
    CHECK("UT-05 path /index.html", str_eq(g_req.path, "/index.html"));
    CHECK("UT-05 version HTTP/1.1", str_eq(g_req.version, "HTTP/1.1"));
    CHECK("UT-05 header_count 1", g_req.header_count == 1);
    CHECK("UT-05 이름 Host", g_req.header_count == 1 && str_eq(g_req.headers[0].name, "Host"));
    CHECK("UT-05 값 localhost", g_req.header_count == 1 && str_eq(g_req.headers[0].value, "localhost"));
}

static void ut06_to_ut08_values(void)
{
    int r = parse(S("GET / HTTP/1.1\r\nHost:   localhost   \r\n\r\n"));
    CHECK("UT-06 값 앞뒤 공백 지움", r == 0 && str_eq(http_find_header(&g_req, "Host"), "localhost"));

    r = parse(S("GET / HTTP/1.1\r\nHost:\tlocalhost\t\r\n\r\n"));
    CHECK("UT-07 값 앞뒤 탭 지움", r == 0 && str_eq(http_find_header(&g_req, "Host"), "localhost"));

    r = parse(S("GET / HTTP/1.1\r\nHost: a.com:8080\r\n\r\n"));
    CHECK("UT-08 첫 :에서만 나눔 (이름)", r == 0 && str_eq(g_req.headers[0].name, "Host"));
    CHECK("UT-08 첫 :에서만 나눔 (값)", r == 0 && str_eq(g_req.headers[0].value, "a.com:8080"));
}

static void ut09_to_ut11_find(void)
{
    int r = parse(S("GET / HTTP/1.1\r\nhost: x\r\n\r\n"));
    CHECK("UT-09 host로 받고 Host로 찾기", r == 0 && str_eq(http_find_header(&g_req, "Host"), "x"));

    r = parse(S("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
    CHECK("UT-10 Host로 받고 HOST로 찾기", r == 0 && str_eq(http_find_header(&g_req, "HOST"), "x"));

    r = parse(S("GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    CHECK("UT-11 없는 헤더는 NULL", r == 0 && http_find_header(&g_req, "Cookie") == NULL);
}

static void ut12_http10_no_host(void)
{
    int r = parse(S("GET / HTTP/1.0\r\n\r\n"));
    CHECK("UT-12 HTTP/1.0은 Host 없어도 0", r == 0);
    CHECK("UT-12 header_count 0", g_req.header_count == 0);
}

static void ut13_32_headers(void)
{
    char req[2048];
    size_t len = build_many_headers(req, sizeof(req), "GET / HTTP/1.1", 31);
    int r = parse(req, len);
    CHECK("UT-13 헤더 32줄은 0", r == 0);
    CHECK("UT-13 header_count 32", g_req.header_count == 32);
}

/* ---- 요청 줄 오류 ---- */

/* 요청 줄 뒤에 Host와 헤더의 끝을 붙여 파싱한다 */
static int parse_line(const char *line, size_t line_len)
{
    char req[512];
    memcpy(req, line, line_len);
    memcpy(req + line_len, "\r\nHost: x\r\n\r\n", 13);
    return parse(req, line_len + 13);
}

static void ut14_to_ut26_request_line(void)
{
    CHECK("UT-14 두 부분 → 400", parse_line(S("GET /")) == 400);
    CHECK("UT-15 네 부분 → 400", parse_line(S("GET / HTTP/1.1 extra")) == 400);
    CHECK("UT-16 공백 두 개 → 400", parse_line(S("GET  / HTTP/1.1")) == 400);
    CHECK("UT-17 메서드 비었음 → 400", parse_line(S(" / HTTP/1.1")) == 400);
    CHECK("UT-18 경로가 /로 시작 안 함 → 400", parse_line(S("GET index.html HTTP/1.1")) == 400);
    CHECK("UT-19 HTTP/1 → 400", parse_line(S("GET / HTTP/1")) == 400);
    CHECK("UT-20 HTTPS/1.1 → 400", parse_line(S("GET / HTTPS/1.1")) == 400);
    CHECK("UT-21 HTTP/a.b → 400", parse_line(S("GET / HTTP/a.b")) == 400);
    CHECK("UT-22 BREW → 501", parse_line(S("BREW / HTTP/1.1")) == 501);
    CHECK("UT-23 POST → 501", parse_line(S("POST / HTTP/1.1")) == 501);
    CHECK("UT-24 소문자 get → 501", parse_line(S("get / HTTP/1.1")) == 501);
    CHECK("UT-25 HTTP/2.0 → 505", parse_line(S("GET / HTTP/2.0")) == 505);
    CHECK("UT-26 HTTP/0.9 → 505", parse_line(S("GET / HTTP/0.9")) == 505);
}

/* ---- 헤더 오류 ---- */

static void ut27_to_ut34_headers(void)
{
    CHECK("UT-27 : 없음 → 400", parse(S("GET / HTTP/1.1\r\nHost x\r\n\r\n")) == 400);
    CHECK("UT-28 이름 비었음 → 400", parse(S("GET / HTTP/1.1\r\n: x\r\n\r\n")) == 400);
    CHECK("UT-29 : 앞 공백 → 400", parse(S("GET / HTTP/1.1\r\nHost : x\r\n\r\n")) == 400);
    CHECK("UT-30 : 앞 탭 → 400", parse(S("GET / HTTP/1.1\r\nHost\t: x\r\n\r\n")) == 400);
    CHECK("UT-31 HTTP/1.1인데 Host 없음 → 400", parse(S("GET / HTTP/1.1\r\n\r\n")) == 400);

    char req[2048];
    size_t len = build_many_headers(req, sizeof(req), "GET / HTTP/1.1", 32);
    CHECK("UT-32 헤더 33줄 → 431", parse(req, len) == 431);

    CHECK("UT-33 헤더 값 안에 \\0 → 400", parse(S("GET / HTTP/1.1\r\nHost: x\0y\r\n\r\n")) == 400);
    CHECK("UT-34 요청 줄 안에 \\0 → 400", parse(S("GET /a\0b HTTP/1.1\r\nHost: x\r\n\r\n")) == 400);
}

/* ---- 검사 순서 ---- */

static void ut35_to_ut38_order(void)
{
    CHECK("UT-35 메서드·버전 둘 다 틀리면 501", parse_line(S("BREW / HTTP/2.0")) == 501);
    CHECK("UT-36 형식도 틀리면 400", parse_line(S("BREW index HTTP/2.0")) == 400);
    CHECK("UT-37 버전·헤더 둘 다 틀리면 505", parse(S("GET / HTTP/2.0\r\nHost x\r\n\r\n")) == 505);

    char req[2048];
    size_t len = build_many_headers(req, sizeof(req), "BREW / HTTP/1.1", 32);
    CHECK("UT-38 메서드 틀려도 헤더 33줄이면 431", parse(req, len) == 431);
}

/* ---- http_reason ---- */

static void ut39_reason(void)
{
    CHECK("UT-39 200 OK", str_eq(http_reason(200), "OK"));
    CHECK("UT-39 400 Bad Request", str_eq(http_reason(400), "Bad Request"));
    CHECK("UT-39 431 Request Header Fields Too Large", str_eq(http_reason(431), "Request Header Fields Too Large"));
    CHECK("UT-39 501 Not Implemented", str_eq(http_reason(501), "Not Implemented"));
    CHECK("UT-39 505 HTTP Version Not Supported", str_eq(http_reason(505), "HTTP Version Not Supported"));
}

int main(void)
{
    printf("test_http\n");
    ut01_to_ut04_header_end();
    ut05_basic();
    ut06_to_ut08_values();
    ut09_to_ut11_find();
    ut12_http10_no_host();
    ut13_32_headers();
    ut14_to_ut26_request_line();
    ut27_to_ut34_headers();
    ut35_to_ut38_order();
    ut39_reason();
    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
