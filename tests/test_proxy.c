/*
 * Step 6 단위 테스트: 백엔드 응답 머리 검증(TC-6-07), 요청·응답 머리 다시 쓰기와 홉 머리(TC-6-08),
 * 204·304·Content-Length 본문 끝 판정(TC-6-09의 순수 함수 부분).
 */
#define _POSIX_C_SOURCE 200809L

#include "../src/proxy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(const char *name, int condition, const char *detail)
{
    if (condition) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (%s)\n", name, detail);
        failures++;
    }
}

static proxy_parse_result parse_s(const char *s, proxy_response *out)
{
    return proxy_parse_response(s, strlen(s), out);
}

static void expect_bad(const char *name, const char *data, size_t len)
{
    proxy_response r;
    char label[200];
    snprintf(label, sizeof(label), "TC-6-07 %s → 502", name);
    check(label, proxy_parse_response(data, len, &r) == PROXY_PARSE_BAD, "BAD가 아니다");
}

static void bad(const char *name, const char *s)
{
    expect_bad(name, s, strlen(s));
}

static void expect_body(const char *name, const char *s, proxy_body body, size_t cl)
{
    proxy_response r;
    char label[200];
    snprintf(label, sizeof(label), "TC-6-09 %s", name);
    check(label, parse_s(s, &r) == PROXY_PARSE_OK && r.body == body && (body != PROXY_BODY_LENGTH || r.content_length == cl),
          "판정이 다르다");
}

/* 응답 머리 len바이트(종료 빈 줄 포함)를 만든다. 채움 머리 X: aaa... 를 넣는다. */
static char *head_of_len(size_t len)
{
    static const char start[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX: ";
    char *p = malloc(len + 16);
    size_t fill = len - (sizeof(start) - 1) - 4;
    memcpy(p, start, sizeof(start) - 1);
    memset(p + sizeof(start) - 1, 'a', fill);
    memcpy(p + len - 4, "\r\n\r\n", 4);
    return p;
}

static void test_parse_valid_and_split(void)
{
    static const char resp[] = "HTTP/1.0 200 OK\r\nContent-Length: 3\r\nServer: py\r\n\r\nabcEXTRA";
    size_t head = sizeof("HTTP/1.0 200 OK\r\nContent-Length: 3\r\nServer: py\r\n\r\n") - 1, i;
    proxy_response r;
    int all_incomplete = 1;

    for (i = 0; i < head; i++) {
        all_incomplete &= proxy_parse_response(resp, i, &r) == PROXY_PARSE_INCOMPLETE;
    }
    check("TC-6-07 머리 완성 전 모든 분할 도착 → 기다림", all_incomplete, "INCOMPLETE가 아닌 길이가 있다");
    check("TC-6-07 머리와 본문 동시 도착 → 머리 길이만",
          proxy_parse_response(resp, sizeof(resp) - 1, &r) == PROXY_PARSE_OK && r.head_len == head && r.status == 200 &&
              r.body == PROXY_BODY_LENGTH && r.content_length == 3,
          "판정이 다르다");
    check("TC-6-07 이유 문구 없음 허용", parse_s("HTTP/1.1 200\r\n\r\n", &r) == PROXY_PARSE_OK, "BAD");
    check("TC-6-07 빈 이유 문구(공백 하나) 허용", parse_s("HTTP/1.1 404 \r\n\r\n", &r) == PROXY_PARSE_OK && r.status == 404,
          "BAD");
    check("TC-6-07 값 안 HTAB·obs-text(0x80~) 허용", parse_s("HTTP/1.1 200 \xc3\xa9\r\nX: a\tb\xff\r\n\r\n", &r) == PROXY_PARSE_OK,
          "BAD");
    check("TC-6-07 값 앞뒤 공백 허용", parse_s("HTTP/1.1 200 OK\r\nX:   a  \r\n\r\n", &r) == PROXY_PARSE_OK, "BAD");
}

static void test_parse_size(void)
{
    proxy_response r;
    char *p = head_of_len(PROXY_RESPONSE_HEAD_MAX);

    check("TC-6-07 머리 정확히 8KB → 통과", proxy_parse_response(p, PROXY_RESPONSE_HEAD_MAX, &r) == PROXY_PARSE_OK, "BAD");
    free(p);
    p = head_of_len(PROXY_RESPONSE_HEAD_MAX + 1);
    expect_bad("머리 8KB + 1", p, PROXY_RESPONSE_HEAD_MAX + 1);
    check("TC-6-07 종료 빈 줄 없이 8191바이트 → 기다림",
          proxy_parse_response(p, PROXY_RESPONSE_HEAD_MAX - 1, &r) == PROXY_PARSE_INCOMPLETE, "INCOMPLETE가 아니다");
    expect_bad("종료 빈 줄 없이 8192바이트", p, PROXY_RESPONSE_HEAD_MAX);
    free(p);
}

static void test_parse_bad(void)
{
    static const char nul[] = "HTTP/1.1 200 OK\r\nX: a\0b\r\n\r\n";
    char many[4096] = "HTTP/1.1 200 OK\r\n";
    proxy_response r;
    int i;

    bad("단독 LF", "HTTP/1.1 200 OK\nX: a\r\n\r\n");
    bad("단독 CR", "HTTP/1.1 200 OK\r\nX: a\rb\r\n\r\n");
    bad("단독 LF(머리 미완성에서도 바로)", "HTTP/1.1 200 OK\n");
    expect_bad("NUL", nul, sizeof(nul) - 1);
    bad("HTTP/1.2", "HTTP/1.2 200 OK\r\n\r\n");
    bad("HTTP/2", "HTTP/2 200\r\n\r\n");
    bad("소문자 http", "http/1.1 200 OK\r\n\r\n");
    bad("상태 99", "HTTP/1.1 099 X\r\n\r\n");
    bad("상태 600", "HTTP/1.1 600 X\r\n\r\n");
    bad("상태 네 자리", "HTTP/1.1 2000 X\r\n\r\n");
    bad("상태 두 자리", "HTTP/1.1 20 X\r\n\r\n");
    bad("버전 뒤 공백 둘", "HTTP/1.1  200 OK\r\n\r\n");
    bad("이유 문구에 제어 문자", "HTTP/1.1 200 O\x01K\r\n\r\n");
    bad("obs-fold", "HTTP/1.1 200 OK\r\nX: a\r\n b\r\n\r\n");
    bad("이름 앞 공백(첫 머리)", "HTTP/1.1 200 OK\r\n X: a\r\n\r\n");
    bad("Content-Length : (이름 뒤 공백)", "HTTP/1.1 200 OK\r\nContent-Length : 3\r\n\r\nabc");
    bad("빈 이름", "HTTP/1.1 200 OK\r\n: a\r\n\r\n");
    bad("이름에 token 밖 문자", "HTTP/1.1 200 OK\r\nX(y): a\r\n\r\n");
    bad("콜론 없음", "HTTP/1.1 200 OK\r\nXa\r\n\r\n");
    bad("값에 제어 문자 0x01", "HTTP/1.1 200 OK\r\nX: a\x01\r\n\r\n");
    bad("값에 DEL 0x7f", "HTTP/1.1 200 OK\r\nX: a\x7f\r\n\r\n");
    bad("1xx 100", "HTTP/1.1 100 Continue\r\n\r\n");
    bad("1xx 101", "HTTP/1.1 101 Switching\r\n\r\n");
    for (i = 0; i < 100; i++) {
        strcat(many, "X: a\r\n");
    }
    strcat(many, "\r\n");
    check("TC-6-07 머리 100줄 → 통과", parse_s(many, &r) == PROXY_PARSE_OK, "BAD");
    strcpy(many + strlen(many) - 2, "X: a\r\n\r\n");
    bad("머리 101줄", many);
}

static void test_parse_framing(void)
{
    bad("Transfer-Encoding (200)", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    bad("Transfer-Encoding (204)", "HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n\r\n");
    bad("Transfer-Encoding (304, 대소문자)", "HTTP/1.1 304 NM\r\ntransfer-encoding: identity\r\n\r\n");
    bad("Content-Length 두 줄(같은 값)", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
    bad("Content-Length 숫자 아님", "HTTP/1.1 200 OK\r\nContent-Length: 3a\r\n\r\n");
    bad("Content-Length 빈 값", "HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n");
    bad("Content-Length +3", "HTTP/1.1 200 OK\r\nContent-Length: +3\r\n\r\n");
    bad("Content-Length 쉼표 목록", "HTTP/1.1 200 OK\r\nContent-Length: 3, 3\r\n\r\n");
    bad("Content-Length 넘침", "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n");
    bad("Connection: Content-Length", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: Content-Length\r\n\r\nabc");
    bad("Connection 토큰 transfer-encoding", "HTTP/1.1 200 OK\r\nConnection: keep-alive , TRANSFER-ENCODING\r\n\r\n");
    bad("204 Content-Length 형식 오류", "HTTP/1.1 204 NC\r\nContent-Length: x\r\n\r\n");
    bad("204 Content-Length 두 줄", "HTTP/1.1 204 NC\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n");
    bad("304 Content-Length 두 줄", "HTTP/1.1 304 NM\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n");
    expect_body("204 길이 없음 → 머리 끝", "HTTP/1.0 204 No Content\r\n\r\n", PROXY_BODY_NONE, 0);
    expect_body("204 Content-Length: 0 → 머리 끝", "HTTP/1.0 204 No Content\r\nContent-Length: 0\r\n\r\n", PROXY_BODY_NONE, 0);
    expect_body("204 Content-Length: 5 → 머리 끝(뒤 바이트 무시)", "HTTP/1.0 204 NC\r\nContent-Length: 5\r\n\r\nhello",
                PROXY_BODY_NONE, 0);
    expect_body("304 Content-Length: 10 → 머리 끝", "HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n",
                PROXY_BODY_NONE, 0);
    expect_body("200 Content-Length: 0 → 0바이트", "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n", PROXY_BODY_LENGTH, 0);
    expect_body("200 Content-Length: 007 → 7바이트", "HTTP/1.0 200 OK\r\ncontent-length: 007\r\n\r\n", PROXY_BODY_LENGTH, 7);
    expect_body("404 길이 없음 → EOF까지", "HTTP/1.0 404 Not Found\r\n\r\n", PROXY_BODY_EOF, 0);
    expect_body("Connection: close 는 프레이밍 아님", "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Length: 1\r\n\r\nx",
                PROXY_BODY_LENGTH, 1);
}

/* raw 요청을 파싱해 백엔드 요청을 만들고 want와 비교한다. */
static void expect_request(const char *name, const char *raw, const char *target, const char *want)
{
    static http_request req;
    char out[16384], label[200];
    int n = -1;

    if (http_parse_request(raw, strlen(raw), &req) == HTTP_PARSE_OK) {
        n = proxy_format_request(out, sizeof(out), &req, target, "h:8001");
    }
    snprintf(label, sizeof(label), "TC-6-08 %s", name);
    check(label, n >= 0 && (size_t)n == strlen(want) && memcmp(out, want, (size_t)n) == 0, n >= 0 ? (out[n] = '\0', out) : "실패");
}

static void test_format_request(void)
{
    static http_request req;
    char small[40];

    expect_request("홉 머리·Host·Expect 빼고 Host·Connection: close 붙임, 같은 이름 보존",
                   "GET /api/users?x=1 HTTP/1.1\r\nHost: client\r\nUser-Agent: u\r\nConnection: keep-alive, X-Hop\r\n"
                   "Keep-Alive: 5\r\nAccept: a\r\nTE: trailers\r\nX-Hop: 1\r\nUpgrade: h2c\r\nProxy-Connection: x\r\n"
                   "Accept: b\r\n\r\n",
                   "/users?x=1",
                   "GET /users?x=1 HTTP/1.0\r\nUser-Agent: u\r\nAccept: a\r\nAccept: b\r\nHost: h:8001\r\nConnection: close\r\n\r\n");
    expect_request("여러 Connection 줄·대소문자·지명 머리가 앞뒤에 있음",
                   "GET / HTTP/1.1\r\nx-before: 1\r\nHost: c\r\nconnection: X-Before\r\nX-Keep: k\r\nCONNECTION:  ,x-after ,\r\n"
                   "X-After: 2\r\n\r\n",
                   "/", "GET / HTTP/1.0\r\nX-Keep: k\r\nHost: h:8001\r\nConnection: close\r\n\r\n");
    expect_request("Connection: Content-Length 이어도 검증한 길이로 새로 씀",
                   "POST /api/x HTTP/1.1\r\nHost: c\r\nContent-Length: 5\r\nConnection: Content-Length\r\n"
                   "Expect: 100-continue\r\n\r\n",
                   "/x", "POST /x HTTP/1.0\r\nHost: h:8001\r\nConnection: close\r\nContent-Length: 5\r\n\r\n");
    expect_request("Content-Length: 0 도 다시 씀",
                   "POST / HTTP/1.0\r\nContent-Length: 0\r\n\r\n", "/",
                   "POST / HTTP/1.0\r\nHost: h:8001\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    expect_request("HTTP/1.0 GET, 길이 없음 → 길이 머리 없음", "GET /a HTTP/1.0\r\nX: y\r\n\r\n", "/a",
                   "GET /a HTTP/1.0\r\nX: y\r\nHost: h:8001\r\nConnection: close\r\n\r\n");
    http_parse_request("GET / HTTP/1.0\r\n\r\n", 18, &req);
    errno = 0;
    check("TC-6-08 버퍼 부족 → -1(EINVAL)",
          proxy_format_request(small, sizeof(small), &req, "/", "h:8001") == -1 && errno == EINVAL, "성공했다");
}

static void expect_response(const char *name, const char *head, http_connection conn, const char *want)
{
    proxy_response r;
    char out[16384], label[200];
    int n = -1;

    if (parse_s(head, &r) == PROXY_PARSE_OK) {
        n = proxy_format_response_head(out, sizeof(out), head, &r, conn);
    }
    snprintf(label, sizeof(label), "TC-6-08 %s", name);
    check(label, n >= 0 && (size_t)n == strlen(want) && memcmp(out, want, (size_t)n) == 0, n >= 0 ? (out[n] = '\0', out) : "실패");
}

static void test_format_response(void)
{
    static const char head[] = "HTTP/1.0 200 OK\r\nServer: s\r\nConnection: close, X-Hop\r\nX-Hop: 1\r\nKeep-Alive: t\r\n"
                               "Content-Length: 3\r\nx-hop: 2\r\nProxy-Connection: k\r\nTE: x\r\nUpgrade: y\r\n\r\n";
    proxy_response r;
    char small[16];

    expect_response("1.1 유지: 버전만 바꾸고 홉 머리 빼고 Connection 없음", head, HTTP_CONNECTION_NONE,
                    "HTTP/1.1 200 OK\r\nServer: s\r\nContent-Length: 3\r\n\r\n");
    expect_response("1.0 유지: Connection: keep-alive", head, HTTP_CONNECTION_KEEP_ALIVE,
                    "HTTP/1.1 200 OK\r\nServer: s\r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\n");
    expect_response("닫기: Connection: close", "HTTP/1.0 404 Not Found\r\nDate: d\r\n\r\n", HTTP_CONNECTION_CLOSE,
                    "HTTP/1.1 404 Not Found\r\nDate: d\r\nConnection: close\r\n\r\n");
    expect_response("204의 Content-Length는 뺌", "HTTP/1.0 204\r\nContent-Length: 0\r\nX: 1\r\n\r\n", HTTP_CONNECTION_NONE,
                    "HTTP/1.1 204\r\nX: 1\r\n\r\n");
    expect_response("304의 Content-Length는 유지", "HTTP/1.1 304 NM\r\nContent-Length: 10\r\n\r\n", HTTP_CONNECTION_NONE,
                    "HTTP/1.1 304 NM\r\nContent-Length: 10\r\n\r\n");
    parse_s(head, &r);
    errno = 0;
    check("TC-6-08 응답 버퍼 부족 → -1(EINVAL)",
          proxy_format_response_head(small, sizeof(small), head, &r, HTTP_CONNECTION_NONE) == -1 && errno == EINVAL,
          "성공했다");
}

int main(void)
{
    printf("test_proxy (TC-6-07, TC-6-08, TC-6-09)\n");
    test_parse_valid_and_split();
    test_parse_size();
    test_parse_bad();
    test_parse_framing();
    test_format_request();
    test_format_response();
    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
