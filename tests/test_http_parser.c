/* Step 2 단위 테스트: http_parser를 소켓 없이 직접 호출한다(TC-2-04, D-15 표·README 3장). */
#include "../src/http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static http_request req; /* 약 10KB라 스택 대신 정적 영역에 둔다 */

static const char *result_name(http_parse_result r)
{
    switch (r) {
    case HTTP_PARSE_INCOMPLETE: return "INCOMPLETE";
    case HTTP_PARSE_OK: return "OK";
    case HTTP_PARSE_BAD_REQUEST: return "400";
    case HTTP_PARSE_NOT_IMPLEMENTED: return "501";
    case HTTP_PARSE_VERSION_NOT_SUPPORTED: return "505";
    }
    return "?";
}

static void check(const char *name, int condition, const char *detail)
{
    if (condition) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (%s)\n", name, detail);
        failures++;
    }
}

/* data[0..len)을 파싱해 기대 결과와 비교한다. 결과를 돌려준다. */
static http_parse_result expect_len(const char *name, const char *data, size_t len,
                                    http_parse_result want)
{
    char detail[64];
    http_parse_result got = http_parse_request(data, len, &req);
    snprintf(detail, sizeof(detail), "기대 %s, 실제 %s", result_name(want), result_name(got));
    check(name, got == want, detail);
    return got;
}

static http_parse_result expect(const char *name, const char *text, http_parse_result want)
{
    return expect_len(name, text, strlen(text), want);
}

static int str_eq(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* prefix + 'a' * fill + suffix를 buf에 만든다. 전체 길이를 돌려준다. */
static size_t build(char *buf, const char *prefix, size_t fill, const char *suffix)
{
    size_t p = strlen(prefix), s = strlen(suffix);
    memcpy(buf, prefix, p);
    memset(buf + p, 'a', fill);
    memcpy(buf + p + fill, suffix, s);
    return p + fill + s;
}

static void test_normal(void)
{
    if (expect("정상 GET 1.1", "GET / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_OK) != HTTP_PARSE_OK) {
        return;
    }
    check("정상 필드 값", str_eq(req.method, "GET") && str_eq(req.target, "/") &&
          str_eq(req.version, "HTTP/1.1") && req.version_minor == 1 && req.header_count == 1 &&
          str_eq(req.headers[0].name, "Host") && str_eq(req.headers[0].value, "x"), "필드가 다르다");
    check("host·HOST로 조회", str_eq(http_request_header(&req, "host"), "x") &&
          str_eq(http_request_header(&req, "HOST"), "x"), "조회 실패");
    check("없는 헤더는 NULL", http_request_header(&req, "Missing") == NULL, "NULL이 아니다");

    expect("POST 1.1", "POST /form HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_OK);
    if (expect("1.0 Host 없음", "GET / HTTP/1.0\r\n\r\n", HTTP_PARSE_OK) == HTTP_PARSE_OK) {
        check("1.0 version_minor 0", req.version_minor == 0 && req.header_count == 0, "필드가 다르다");
    }
    if (expect("값 앞뒤 공백·빈 값", "GET /a?b=c HTTP/1.1\r\nHost: \t x y \t\r\nX-Empty:\r\n\r\n",
               HTTP_PARSE_OK) == HTTP_PARSE_OK) {
        check("값 앞뒤 공백 제거, 빈 값 허용", str_eq(req.target, "/a?b=c") &&
              str_eq(http_request_header(&req, "host"), "x y") &&
              str_eq(http_request_header(&req, "x-empty"), ""), "값이 다르다");
    }
    if (expect("같은 이름 헤더 첫 값", "GET / HTTP/1.1\r\nHost: x\r\nA: 1\r\na: 2\r\n\r\n",
               HTTP_PARSE_OK) == HTTP_PARSE_OK) {
        check("첫 번째 값을 돌려줌", str_eq(http_request_header(&req, "A"), "1") && req.header_count == 3,
              "첫 값이 아니다");
    }
}

static void test_host_rules(void)
{
    expect("1.1 Host 없음", "GET / HTTP/1.1\r\nA: b\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("Host 2개", "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("Host와 host", "GET / HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("1.0도 Host 2개는 400", "GET / HTTP/1.0\r\nHost: a\r\nHOST: b\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
}

static void test_split(void)
{
    const char full[] = "GET /path HTTP/1.1\r\nHost: x\r\nUser-Agent: t\r\n\r\n";
    size_t n = strlen(full), k;
    int all_incomplete = 1, saw_cr_end = 0;

    for (k = 0; k < n; k++) {
        if (http_parse_request(full, k, &req) != HTTP_PARSE_INCOMPLETE) {
            printf("    앞부분 %zu바이트가 INCOMPLETE가 아니다\n", k);
            all_incomplete = 0;
        }
        if (k > 0 && full[k - 1] == '\r') {
            saw_cr_end = 1;
        }
    }
    check("분할: 모든 앞부분 INCOMPLETE", all_incomplete && saw_cr_end, "INCOMPLETE가 아닌 앞부분이 있다");
    if (expect("분할: 전체 OK", full, HTTP_PARSE_OK) == HTTP_PARSE_OK) {
        check("분할: 전체 필드", str_eq(req.target, "/path") &&
              str_eq(http_request_header(&req, "user-agent"), "t"), "필드가 다르다");
    }
}

static void test_request_line(void)
{
    expect("요청 줄 공백 2개", "GET  / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("요청 줄 부분 누락", "GET /\r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("요청 줄 끝 공백", "GET / HTTP/1.1 \r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("첫 줄이 빈 줄", "\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("종료 빈 줄 안의 맨 \\n", "GET / HTTP/1.1\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("메서드에 tchar 아닌 문자", "G(T / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("대상에 제어 문자", "GET /\x01 HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("요청 줄 탭 구분", "GET\t/ HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
}

static void test_line_breaks(void)
{
    static const char nul_mid[] = "GET / HT\0TP/1.1\r\n";
    static const char after_head[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n\nab\0c\r";

    expect("종료 빈 줄 없이 맨 \\n만 (즉시 400)", "GET / HTTP/1.1\nHost: x\n\n", HTTP_PARSE_BAD_REQUEST);
    expect("맨 \\n 하나로 즉시 400", "GET / HTTP/1.1\n", HTTP_PARSE_BAD_REQUEST);
    expect_len("머리 도중 NUL", nul_mid, sizeof(nul_mid) - 1, HTTP_PARSE_BAD_REQUEST);
    expect_len("완성된 머리 안 NUL", "GET / HTTP/1.1\r\nHost: \0\r\n\r\n", 27, HTTP_PARSE_BAD_REQUEST);
    expect("\\n 없는 \\r (GET /\\rX)", "GET /\rX", HTTP_PARSE_BAD_REQUEST);
    expect("마지막 \\r은 기다림", "GET / HTTP/1.1\r", HTTP_PARSE_INCOMPLETE);
    expect_len("종료 빈 줄 뒤 맨 \\n·NUL은 보지 않음", after_head, sizeof(after_head) - 1, HTTP_PARSE_OK);
}

static void test_version(void)
{
    expect("버전 HTTP/1", "GET / HTTP/1\r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("버전 http/1.1", "GET / http/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("버전 HTTP/1.10", "GET / HTTP/1.10\r\nHost: x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("버전 HTTP/2.0", "GET / HTTP/2.0\r\nHost: x\r\n\r\n", HTTP_PARSE_VERSION_NOT_SUPPORTED);
    expect("버전 HTTP/1.2", "GET / HTTP/1.2\r\nHost: x\r\n\r\n", HTTP_PARSE_VERSION_NOT_SUPPORTED);
    expect("버전 HTTP/0.9", "GET / HTTP/0.9\r\nHost: x\r\n\r\n", HTTP_PARSE_VERSION_NOT_SUPPORTED);
}

static void test_method(void)
{
    expect("메서드 BREW", "BREW / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_NOT_IMPLEMENTED);
    expect("메서드 HEAD", "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_NOT_IMPLEMENTED);
    expect("메서드 get", "get / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_PARSE_NOT_IMPLEMENTED);
    expect("BREW / HTTP/2.0은 505", "BREW / HTTP/2.0\r\nHost: x\r\n\r\n", HTTP_PARSE_VERSION_NOT_SUPPORTED);
    expect("BREW에 Host 없어도 501", "BREW / HTTP/1.1\r\n\r\n", HTTP_PARSE_NOT_IMPLEMENTED);
}

static void test_header_syntax(void)
{
    expect("헤더 콜론 없음", "GET / HTTP/1.1\r\nHost x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("헤더 이름 뒤 공백", "GET / HTTP/1.1\r\nHost : x\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("헤더 obs-fold", "GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("헤더 이름 비어 있음", "GET / HTTP/1.1\r\nHost: x\r\n: v\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
}

/* Host 1개 + X<i> 헤더로 count개를 만든다. */
static size_t build_headers(char *buf, size_t count)
{
    size_t len = (size_t)sprintf(buf, "GET / HTTP/1.1\r\nHost: x\r\n");
    size_t i;
    for (i = 1; i < count; i++) {
        len += (size_t)sprintf(buf + len, "X%zu: v\r\n", i);
    }
    len += (size_t)sprintf(buf + len, "\r\n");
    return len;
}

static void test_header_count(void)
{
    static char buf[4096];
    size_t len = build_headers(buf, 100);
    if (expect_len("헤더 100개", buf, len, HTTP_PARSE_OK) == HTTP_PARSE_OK) {
        check("헤더 100개 모두 보관", req.header_count == 100 &&
              str_eq(req.headers[99].name, "X99"), "개수가 다르다");
    }
    len = build_headers(buf, 101);
    expect_len("헤더 101개", buf, len, HTTP_PARSE_BAD_REQUEST);
}

static void test_8k_boundary(void)
{
    static char buf[HTTP_REQUEST_HEAD_MAX + 64];
    const char *prefix = "GET / HTTP/1.1\r\nHost: x\r\nX: ";
    size_t p = strlen(prefix), len;

    len = build(buf, prefix, HTTP_REQUEST_HEAD_MAX - p - 4, "\r\n\r\n");
    check("8KB: 길이 준비 8192", len == 8192, "길이가 다르다");
    if (expect_len("8KB: 종료 포함 8192 OK", buf, len, HTTP_PARSE_OK) == HTTP_PARSE_OK) {
        check("8KB: 긴 값 복사", strlen(http_request_header(&req, "x")) == HTTP_REQUEST_HEAD_MAX - p - 4,
              "값 길이가 다르다");
    }
    memcpy(buf + len, "xyz", 3);
    expect_len("8KB: 종료 포함 8192 + 뒤 바이트 OK", buf, len + 3, HTTP_PARSE_OK);

    memset(buf, 'a', HTTP_REQUEST_HEAD_MAX);
    expect_len("8KB: 종료 없이 8191 INCOMPLETE", buf, 8191, HTTP_PARSE_INCOMPLETE);
    expect_len("8KB: 종료 없이 8192 400", buf, 8192, HTTP_PARSE_BAD_REQUEST);
    expect_len("8KB: 종료 없이 8200 400", buf, 8200, HTTP_PARSE_BAD_REQUEST);

    len = build(buf, prefix, HTTP_REQUEST_HEAD_MAX - p - 3, "\r\n\r");
    check("8KB: 길이 준비 \\r\\n\\r 8192", len == 8192, "길이가 다르다");
    expect_len("8KB: 8192이고 \\r\\n\\r로 끝남 400", buf, len, HTTP_PARSE_BAD_REQUEST);

    len = build(buf, prefix, HTTP_REQUEST_HEAD_MAX - p - 3, "\r\n\r\n");
    check("8KB: 길이 준비 8193", len == 8193, "길이가 다르다");
    expect_len("8KB: 종료 포함 8193 400", buf, len, HTTP_PARSE_BAD_REQUEST);
}

static void test_copy(void)
{
    char input[] = "POST /copy HTTP/1.1\r\nHost: example\r\nX-K: v1\r\n\r\n";
    if (expect("복사: 파싱", input, HTTP_PARSE_OK) != HTTP_PARSE_OK) {
        return;
    }
    memset(input, 'Z', sizeof(input) - 1);
    check("복사: 입력 덮어쓴 뒤에도 그대로", str_eq(req.method, "POST") && str_eq(req.target, "/copy") &&
          str_eq(req.version, "HTTP/1.1") && str_eq(http_request_header(&req, "host"), "example") &&
          str_eq(http_request_header(&req, "x-k"), "v1"), "값이 바뀌었다");
}

int main(void)
{
    printf("test_http_parser (TC-2-04)\n");
    test_normal();
    test_host_rules();
    test_split();
    test_request_line();
    test_line_breaks();
    test_version();
    test_method();
    test_header_syntax();
    test_header_count();
    test_8k_boundary();
    test_copy();

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
