/*
 * mime.c 단위 테스트.
 * 실행: make test   (실패가 있으면 종료 코드 1)
 */
#include "mime.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

/* 경로의 MIME 타입이 기대값과 다르면 줄 번호와 함께 출력하고 실패로 센다 */
#define CHECK_MIME(path, expected)                                          \
    do {                                                                    \
        const char *got_ = mime_type(path);                                 \
        g_checks++;                                                         \
        if (strcmp(got_, (expected)) != 0) {                                \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: mime_type(\"%s\") = \"%s\", want \"%s\"\n", \
                    __FILE__, __LINE__, (path), got_, (expected));          \
        }                                                                   \
    } while (0)

#define OCTET "application/octet-stream"

int main(void)
{
    /* www/ 테스트 파일들 */
    CHECK_MIME("/app/www/index.html", "text/html; charset=utf-8");
    CHECK_MIME("/app/www/style.css",  "text/css; charset=utf-8");
    CHECK_MIME("/app/www/app.js",     "text/javascript; charset=utf-8");
    CHECK_MIME("/app/www/image.png",  "image/png");
    CHECK_MIME("/app/www/photo.jpg",  "image/jpeg");
    CHECK_MIME("/app/www/secret.txt", "text/plain; charset=utf-8");
    CHECK_MIME("/app/www/large.bin",  OCTET);   /* 모르는 확장자 */

    /* 표의 나머지 */
    CHECK_MIME("a.htm",  "text/html; charset=utf-8");
    CHECK_MIME("a.json", "application/json");
    CHECK_MIME("a.jpeg", "image/jpeg");
    CHECK_MIME("a.gif",  "image/gif");
    CHECK_MIME("a.svg",  "image/svg+xml");
    CHECK_MIME("a.ico",  "image/x-icon");

    /* 대소문자 구분 안 함 */
    CHECK_MIME("PHOTO.JPG", "image/jpeg");
    CHECK_MIME("Index.Html", "text/html; charset=utf-8");

    /* 마지막 '.' 뒤가 확장자 */
    CHECK_MIME("archive.tar.gz", OCTET);
    CHECK_MIME("style.min.css", "text/css; charset=utf-8");

    /* 확장자 없음 */
    CHECK_MIME("/app/www/README", OCTET);
    CHECK_MIME("file.", OCTET);
    CHECK_MIME("/app/www/.env", OCTET);       /* 점으로 시작하는 숨김 파일 */
    CHECK_MIME("/app/www/.html", OCTET);
    CHECK_MIME("/v1.2/readme", OCTET);        /* 디렉터리 이름의 '.'은 보지 않음 */
    CHECK_MIME("/v1.2/", OCTET);
    CHECK_MIME("", OCTET);

    printf("test_mime: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
