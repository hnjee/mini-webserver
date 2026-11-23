/*
 * request.c 단위 테스트. 소켓 없이 문자열만 넣어서 파서를 확인한다.
 * 실행: make test   (실패가 있으면 종료 코드 1)
 */
#include "request.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

/* 조건이 거짓이면 줄 번호와 조건식을 출력하고 실패로 센다 */
#define CHECK(cond)                                                     \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

/* 문자열 두 개가 같은지 (NULL이면 다르다고 본다) */
#define STR_EQ(a, b) ((a) != NULL && strcmp((a), (b)) == 0)

/*
 * 서버와 같은 순서로 요청을 처리한다: 헤더 끝을 찾고 → 그 길이만큼 파싱.
 * req의 문자열은 buf를 가리키므로 buf는 정적 배열로 둔다 (다음 호출 전까지 유지).
 * 헤더 끝을 못 찾으면 -1을 돌려준다.
 */
static int parse_str(const char *s, struct http_request *req)
{
    static char buf[REQUEST_BUF_SIZE];
    size_t len = strlen(s);
    ssize_t end;

    if (len > sizeof(buf))
        return -1;
    memcpy(buf, s, len);
    end = request_find_end(buf, len);
    if (end < 0)
        return -1;
    return parse_request(buf, (size_t)end, req);
}

static void test_find_end(void)
{
    CHECK(request_find_end("", 0) == -1);
    CHECK(request_find_end("GET / HTTP/1.1\r\nHost: x\r\n", 25) == -1);
    CHECK(request_find_end("GET / HTTP/1.1\r\n\r", 17) == -1);   /* 한 글자 모자람 */

    /* "\r\n\r\n" 바로 뒤 위치(= 헤더 길이)를 돌려준다. 뒤에 딸려 온 바이트는 무시 */
    const char *s = "GET / HTTP/1.0\r\n\r\nhello";
    CHECK(request_find_end(s, strlen(s)) == 18);
}

static void test_ok(void)
{
    struct http_request req;

    CHECK(parse_str("GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n", &req) == 0);
    CHECK(STR_EQ(req.method, "GET"));
    CHECK(STR_EQ(req.uri, "/index.html"));
    CHECK(STR_EQ(req.version, "HTTP/1.1"));
    CHECK(STR_EQ(req.host, "localhost"));
    CHECK(req.content_length == -1);
    CHECK(req.connection == NULL);

    /* HTTP/1.0은 Host가 없어도 된다 */
    CHECK(parse_str("GET / HTTP/1.0\r\n\r\n", &req) == 0);
    CHECK(req.host == NULL);

    /* POST도 아는 메서드다 (본문은 Step 4) */
    CHECK(parse_str("POST /api HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n", &req) == 0);
    CHECK(STR_EQ(req.method, "POST"));
    CHECK(req.content_length == 5);

    /* 헤더 이름은 대소문자 무관, 값 앞뒤 공백은 잘라낸다, 모르는 헤더는 버린다 */
    CHECK(parse_str("GET / HTTP/1.1\r\nhost: x\r\nCONTENT-LENGTH: 0\r\n"
                    "connection:   close  \r\nUser-Agent: curl\r\n\r\n", &req) == 0);
    CHECK(STR_EQ(req.host, "x"));
    CHECK(req.content_length == 0);
    CHECK(STR_EQ(req.connection, "close"));
}

static void test_400(void)
{
    struct http_request req;

    CHECK(parse_str("GET / HTTP/1.1\r\n\r\n", &req) == 400);              /* Host 없음 */
    CHECK(parse_str("GET /\r\nHost: x\r\n\r\n", &req) == 400);            /* 버전 없음 */
    CHECK(parse_str("GET\r\nHost: x\r\n\r\n", &req) == 400);              /* 한 조각 */
    CHECK(parse_str("GET / HTTP/1.1 x\r\nHost: x\r\n\r\n", &req) == 400); /* 네 조각 */
    CHECK(parse_str("GET  / HTTP/1.1\r\nHost: x\r\n\r\n", &req) == 400);  /* 빈 조각 */
    CHECK(parse_str("\r\n\r\n", &req) == 400);                            /* 빈 요청 라인 */
    CHECK(parse_str("GET index.html HTTP/1.1\r\nHost: x\r\n\r\n", &req) == 400);
    CHECK(parse_str("GET / HTTX/1.1\r\nHost: x\r\n\r\n", &req) == 400);   /* 버전 형식 아님 */
    CHECK(parse_str("GET / HTTP/1.1\r\nHost x\r\n\r\n", &req) == 400);    /* ':' 없음 */
    CHECK(parse_str("GET / HTTP/1.1\r\n: x\r\n\r\n", &req) == 400);       /* 이름 없음 */
    CHECK(parse_str("GET / HTTP/1.1\r\nHost : x\r\n\r\n", &req) == 400);  /* 이름에 공백 */
    CHECK(parse_str("GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n", &req) == 400);

    /* Content-Length는 0 이상의 정수만 */
    CHECK(parse_str("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", &req) == 400);
    CHECK(parse_str("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", &req) == 400);
    CHECK(parse_str("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n", &req) == 400);
    CHECK(parse_str("GET / HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n", &req) == 400);
    CHECK(parse_str("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1 2\r\n\r\n", &req) == 400);
    CHECK(parse_str("GET / HTTP/1.1\r\nHost: x\r\n"
                    "Content-Length: 99999999999999999999999\r\n\r\n", &req) == 400);

    /* 줄 안의 '\n' 단독이나 '\0'은 형식 오류 */
    CHECK(parse_str("GET / HTTP/1.1\nHost: x\r\n\r\n", &req) == 400);
}

static void test_400_with_nul(void)
{
    /* 문자열 리터럴로는 '\0'을 넣을 수 없어서 직접 버퍼를 만든다 */
    char buf[] = "GET / HTTP/1.1\r\nHost: x\0y\r\n\r\n";
    size_t len = sizeof(buf) - 1;
    struct http_request req;
    ssize_t end = request_find_end(buf, len);

    CHECK(end == (ssize_t)len);
    CHECK(parse_request(buf, (size_t)end, &req) == 400);
}

static void test_501_505(void)
{
    struct http_request req;

    CHECK(parse_str("BREW / HTTP/1.1\r\nHost: x\r\n\r\n", &req) == 501);
    CHECK(parse_str("get / HTTP/1.1\r\nHost: x\r\n\r\n", &req) == 501);   /* 대소문자 구분 */
    CHECK(parse_str("DELETE / HTTP/1.1\r\nHost: x\r\n\r\n", &req) == 501);
    CHECK(parse_str("GET / HTTP/2.0\r\nHost: x\r\n\r\n", &req) == 505);
    CHECK(parse_str("GET / HTTP/0.9\r\nHost: x\r\n\r\n", &req) == 505);
}

static void test_order(void)
{
    struct http_request req;

    /* 형식(400) → 메서드(501) → 버전(505) */
    CHECK(parse_str("BREW /\r\nHost: x\r\n\r\n", &req) == 400);
    CHECK(parse_str("BREW / HTTP/2.0\r\nHost: x\r\n\r\n", &req) == 501);
    CHECK(parse_str("GET / HTTP/2.0\r\n\r\n", &req) == 505);   /* Host 검사는 1.1일 때만 */
}

int main(void)
{
    test_find_end();
    test_ok();
    test_400();
    test_400_with_nul();
    test_501_505();
    test_order();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
