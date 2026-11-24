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
    case HTTP_PARSE_URI_TOO_LONG: return "414";
    case HTTP_PARSE_HEADER_TOO_LARGE: return "431";
    case HTTP_PARSE_CONTENT_TOO_LARGE: return "413";
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
    expect_len("헤더 101개 → 431 (D-42)", buf, len, HTTP_PARSE_HEADER_TOO_LARGE);
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
    expect_len("8KB: 종료 없이 8192 431 (D-42)", buf, 8192, HTTP_PARSE_HEADER_TOO_LARGE);
    expect_len("8KB: 종료 없이 8200 431", buf, 8200, HTTP_PARSE_HEADER_TOO_LARGE);

    len = build(buf, prefix, HTTP_REQUEST_HEAD_MAX - p - 3, "\r\n\r");
    check("8KB: 길이 준비 \\r\\n\\r 8192", len == 8192, "길이가 다르다");
    expect_len("8KB: 8192이고 \\r\\n\\r로 끝남 431", buf, len, HTTP_PARSE_HEADER_TOO_LARGE);

    len = build(buf, prefix, HTTP_REQUEST_HEAD_MAX - p - 3, "\r\n\r\n");
    check("8KB: 길이 준비 8193", len == 8193, "길이가 다르다");
    expect_len("8KB: 종료 포함 8193 431", buf, len, HTTP_PARSE_HEADER_TOO_LARGE);
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

/* "GET /" + 'a'*(target_len-1) + suffix. 대상 길이가 정확히 target_len이다. */
static size_t build_target(char *buf, const char *method, size_t target_len, const char *suffix)
{
    size_t m = strlen(method);
    memcpy(buf, method, m);
    buf[m] = ' ';
    return m + 1 + build(buf + m + 1, "/", target_len - 1, suffix);
}

/* TC-4-11: 대상 2048 통과, 2049 414, 종료 빈 줄 없이도 414. 머리 길이 필드(D-47). */
static void test_uri_limit(void)
{
    static char buf[HTTP_REQUEST_HEAD_MAX + 64];
    size_t len;

    len = build_target(buf, "GET", HTTP_REQUEST_TARGET_MAX, " HTTP/1.1\r\nHost: x\r\n\r\n");
    if (expect_len("TC-4-11 대상 2048바이트 OK", buf, len, HTTP_PARSE_OK) == HTTP_PARSE_OK) {
        check("TC-4-11 대상 2048바이트 그대로 복사", strlen(req.target) == HTTP_REQUEST_TARGET_MAX, "길이가 다르다");
    }
    len = build_target(buf, "GET", HTTP_REQUEST_TARGET_MAX + 1, " HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_len("TC-4-11 대상 2049바이트 414", buf, len, HTTP_PARSE_URI_TOO_LONG);
    len = build_target(buf, "GET", HTTP_REQUEST_TARGET_MAX + 1, "");
    expect_len("TC-4-11 종료 빈 줄 없이 대상 2049바이트 → 바로 414", buf, len, HTTP_PARSE_URI_TOO_LONG);
    len = build_target(buf, "GET", HTTP_REQUEST_TARGET_MAX, "");
    expect_len("TC-4-11 종료 빈 줄 없이 대상 2048바이트 → INCOMPLETE", buf, len, HTTP_PARSE_INCOMPLETE);
    len = build_target(buf, "GET", HTTP_REQUEST_TARGET_MAX + 1, "?q HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_len("TC-4-11 쿼리 포함 길이로 센다", buf, len, HTTP_PARSE_URI_TOO_LONG);

    if (expect("TC-4-11 머리 길이 = 종료 빈 줄 포함", "POST /e HTTP/1.1\r\nHost: x\r\n\r\nbody",
               HTTP_PARSE_OK) == HTTP_PARSE_OK) {
        check("TC-4-11 head_len 29", req.head_len == 29, "머리 길이가 다르다");
    }
}

/* 모든 앞부분이 INCOMPLETE이거나 전체와 같은 결과인가(나눠 도착해도 결과 같음, D-48). */
static int split_invariant(const char *data, size_t len, http_parse_result want)
{
    size_t k;
    for (k = 1; k <= len; k++) {
        http_parse_result r = http_parse_request(data, k, &req);
        if (r != HTTP_PARSE_INCOMPLETE && r != want) {
            printf("    앞부분 %zu바이트 결과 %s, 기대 %s\n", k, result_name(r), result_name(want));
            return 0;
        }
    }
    return http_parse_request(data, len, &req) == want;
}

/* TC-4-12: D-48 판정 순서. 앞 순서의 결과가 이긴다. */
static void test_order(void)
{
    static char buf[HTTP_REQUEST_HEAD_MAX + 64];
    size_t len;

    len = build_target(buf, "BREW", 3000, " HTTP/2.0\r\n\r\n");
    expect_len("TC-4-12 1>4: BREW /<3000> HTTP/2.0 → 414", buf, len, HTTP_PARSE_URI_TOO_LONG);
    check("TC-4-12 1>4 나눠 도착해도 414", split_invariant(buf, len, HTTP_PARSE_URI_TOO_LONG), "결과가 다르다");

    len = build_target(buf, "GET", 3000, "\n");
    buf[100] = '\n';
    expect_len("TC-4-12 2>1: 대상 안 맨 \\n이 먼저 → 400", buf, len, HTTP_PARSE_BAD_REQUEST);
    check("TC-4-12 2>1 나눠 도착해도 400", split_invariant(buf, len, HTTP_PARSE_BAD_REQUEST), "결과가 다르다");

    len = build(buf, "GET / HTTP/1.1", 3000, "\r\nHost: x\r\n\r\n");
    expect_len("TC-4-12 버전 자리가 길면 대상은 / → 414 아님(400)", buf, len, HTTP_PARSE_BAD_REQUEST);

    len = build(buf, "GET / HTTP/1.1\r\nX: ", HTTP_REQUEST_HEAD_MAX - 19, "");
    check("TC-4-12 3: 길이 준비 8192", len == HTTP_REQUEST_HEAD_MAX, "길이가 다르다");
    check("TC-4-12 3: 8KB 종료 없음 나눠 도착해도 431", split_invariant(buf, len, HTTP_PARSE_HEADER_TOO_LARGE),
          "결과가 다르다");
    buf[8000] = '\n';
    check("TC-4-12 2>3: 8KB 안 맨 \\n → 나눠 도착해도 400", split_invariant(buf, len, HTTP_PARSE_BAD_REQUEST),
          "결과가 다르다");
    buf[8000] = 'a';
    buf[len - 1] = '\0';
    expect_len("TC-4-12 2>3: 8KB 마지막 바이트 NUL → 400", buf, len, HTTP_PARSE_BAD_REQUEST);

    expect("TC-4-12 4: 형식 400", "GET / HTTP/1.1\r\nHost x\r\nContent-Length: 9999999\r\n\r\n",
           HTTP_PARSE_BAD_REQUEST);
    expect("TC-4-12 4: 505", "GET / HTTP/2.0\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
           HTTP_PARSE_VERSION_NOT_SUPPORTED);
    expect("TC-4-12 4: 501", "HEAD / HTTP/1.1\r\nHost: x\r\nContent-Length: x\r\n\r\n", HTTP_PARSE_NOT_IMPLEMENTED);
    expect("TC-4-12 4>5: Host 없음이 전송 코딩보다 먼저 → 400",
           "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("TC-4-12 5>6: 전송 코딩 chunked가 길이 형식보다 먼저 → 501",
           "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\nTransfer-Encoding: chunked\r\n\r\n",
           HTTP_PARSE_NOT_IMPLEMENTED);
    expect("TC-4-12 5>7: 전송 코딩이 1MB 초과보다 먼저 → 501",
           "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\nTransfer-Encoding: chunked\r\n\r\n",
           HTTP_PARSE_NOT_IMPLEMENTED);
    expect("TC-4-12 6>7: 중복 길이(값이 커도) → 400",
           "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\nContent-Length: 2000000\r\n\r\n",
           HTTP_PARSE_BAD_REQUEST);
    expect("TC-4-12 7: 1MB 초과 → 413", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n",
           HTTP_PARSE_CONTENT_TOO_LARGE);
    {
        const char ok[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
        check("TC-4-12 8: 정상 요청 나눠 도착해도 OK", split_invariant(ok, sizeof(ok) - 1, HTTP_PARSE_OK),
              "결과가 다르다");
    }
}

/* TC-4-13: Content-Length 규칙(D-49). value가 NULL이면 헤더 없음. */
static void expect_length(const char *name, const char *value, http_parse_result want, size_t want_len)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "POST / HTTP/1.1\r\nHost: x\r\n%s%s%s\r\n", value ? "Content-Length:" : "",
             value ? value : "", value ? "\r\n" : "");
    if (expect(name, buf, want) == HTTP_PARSE_OK) {
        char label[160];
        snprintf(label, sizeof(label), "%s 길이 %zu", name, want_len);
        check(label, req.content_length == want_len, "길이가 다르다");
    }
}

static void test_content_length(void)
{
    expect_length("TC-4-13 없음 → 0", NULL, HTTP_PARSE_OK, 0);
    expect_length("TC-4-13 0", " 0", HTTP_PARSE_OK, 0);
    expect_length("TC-4-13 5", " 5", HTTP_PARSE_OK, 5);
    expect_length("TC-4-13 앞의 0 허용", " 0005", HTTP_PARSE_OK, 5);
    expect_length("TC-4-13 1048576 통과", " 1048576", HTTP_PARSE_OK, 1048576);
    expect_length("TC-4-13 1048577 → 413", " 1048577", HTTP_PARSE_CONTENT_TOO_LARGE, 0);
    expect_length("TC-4-13 긴 자릿수 → 413", " 99999999999999999999999999999999999999", HTTP_PARSE_CONTENT_TOO_LARGE,
                  0);
    expect_length("TC-4-13 앞 0이 많아도 값으로 판단", " 0000000000000000000000000000000000000001", HTTP_PARSE_OK, 1);
    expect_length("TC-4-13 빈 값", "", HTTP_PARSE_BAD_REQUEST, 0);
    expect_length("TC-4-13 + 부호", " +5", HTTP_PARSE_BAD_REQUEST, 0);
    expect_length("TC-4-13 - 부호", " -5", HTTP_PARSE_BAD_REQUEST, 0);
    expect_length("TC-4-13 숫자 아님", " 5a", HTTP_PARSE_BAD_REQUEST, 0);
    expect_length("TC-4-13 가운데 공백", " 5 5", HTTP_PARSE_BAD_REQUEST, 0);
    expect_length("TC-4-13 쉼표 목록", " 5, 5", HTTP_PARSE_BAD_REQUEST, 0);
    expect_length("TC-4-13 같은 값 두 번", " 5\r\ncontent-length: 5", HTTP_PARSE_BAD_REQUEST, 0);
    expect("TC-4-13 GET에도 길이 적용", "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\n", HTTP_PARSE_OK);
    check("TC-4-13 GET 길이 3", req.content_length == 3, "길이가 다르다");
}

/* TC-4-21: Transfer-Encoding(D-48 순서 5). */
static void test_transfer_encoding(void)
{
    expect("TC-4-21 chunked → 501", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
           HTTP_PARSE_NOT_IMPLEMENTED);
    expect("TC-4-21 gzip, chunked → 501", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
           HTTP_PARSE_NOT_IMPLEMENTED);
    expect("TC-4-21 chunked, gzip → 400", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
           HTTP_PARSE_BAD_REQUEST);
    expect("TC-4-21 Content-Length와 함께 → 501",
           "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
           HTTP_PARSE_NOT_IMPLEMENTED);
    expect("TC-4-21 빈 값 → 400", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding:\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
    expect("TC-4-21 쉼표만 → 400", "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: , ,\r\n\r\n",
           HTTP_PARSE_BAD_REQUEST);
    expect("TC-4-21 대소문자 CHUNKED, 빈 요소 건너뜀 → 501",
           "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip ,, CHUNKED ,\r\n\r\n", HTTP_PARSE_NOT_IMPLEMENTED);
    expect("TC-4-21 헤더 여러 개 이어 마지막 → 501",
           "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
           HTTP_PARSE_NOT_IMPLEMENTED);
    expect("TC-4-21 헤더 여러 개, 마지막이 gzip → 400",
           "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n\r\n",
           HTTP_PARSE_BAD_REQUEST);
    expect("TC-4-21 chunked;x=1은 chunked 아님 → 400",
           "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked;x=1\r\n\r\n", HTTP_PARSE_BAD_REQUEST);
}

/* TC-4-14: Connection 토큰과 버전으로 유지 여부(D-51 표 2~5행). */
static void expect_keep(const char *name, const char *version, const char *headers, int want)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "GET / %s\r\nHost: x\r\n%s\r\n", version, headers);
    if (http_parse_request(buf, strlen(buf), &req) != HTTP_PARSE_OK) {
        check(name, 0, "파싱 실패");
        return;
    }
    check(name, http_request_keep_alive(&req) == want, want ? "유지가 아니다" : "닫기가 아니다");
}

static void test_connection_tokens(void)
{
    expect_keep("TC-4-14 1.1 헤더 없음 → 유지", "HTTP/1.1", "", 1);
    expect_keep("TC-4-14 1.1 close → 닫기", "HTTP/1.1", "Connection: close\r\n", 0);
    expect_keep("TC-4-14 1.1 CLOSE 대소문자 → 닫기", "HTTP/1.1", "Connection: CLOSE\r\n", 0);
    expect_keep("TC-4-14 1.1 목록 안 close → 닫기", "HTTP/1.1", "Connection: Upgrade ,  close , x\r\n", 0);
    expect_keep("TC-4-14 1.1 두 번째 헤더의 close → 닫기", "HTTP/1.1", "Connection: x\r\nconnection: close\r\n", 0);
    expect_keep("TC-4-14 1.1 closed는 close 아님 → 유지", "HTTP/1.1", "Connection: closed, xclose\r\n", 1);
    expect_keep("TC-4-14 1.1 keep-alive → 유지", "HTTP/1.1", "Connection: keep-alive\r\n", 1);
    expect_keep("TC-4-14 1.0 헤더 없음 → 닫기", "HTTP/1.0", "", 0);
    expect_keep("TC-4-14 1.0 keep-alive → 유지", "HTTP/1.0", "Connection: keep-alive\r\n", 1);
    expect_keep("TC-4-14 1.0 Keep-Alive, Upgrade → 유지", "HTTP/1.0", "Connection: Keep-Alive, Upgrade\r\n", 1);
    expect_keep("TC-4-14 1.0 두 번째 헤더의 keep-alive → 유지", "HTTP/1.0", "Connection: x\r\nConnection: KEEP-ALIVE\r\n",
                1);
    expect_keep("TC-4-14 1.0 keep-alive와 close → 닫기", "HTTP/1.0", "Connection: keep-alive, close\r\n", 0);
    expect_keep("TC-4-14 1.1 keep-alive와 close → 닫기", "HTTP/1.1", "Connection: close\r\nConnection: keep-alive\r\n",
                0);
    expect_keep("TC-4-14 1.0 keep-alives는 아님 → 닫기", "HTTP/1.0", "Connection: keep-alives\r\n", 0);
}

int main(void)
{
    printf("test_http_parser (TC-2-04, TC-4-11~14, TC-4-21)\n");
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
    test_uri_limit();
    test_order();
    test_content_length();
    test_transfer_encoding();
    test_connection_tokens();

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
