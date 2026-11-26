/*
 * Step 3 경로·MIME 단위 테스트: docs/tests/step3.md 3장 UT-01 ~ UT-31
 * resolve_path()는 /tmp 아래에 테스트용 루트를 만들어 확인한다.
 *
 *   <base>/www/index.html, style.css, secret.txt(권한 000)
 *   <base>/www/sub/index.html
 *   <base>/www/empty/            (index.html 없음)
 *   <base>/www/link  -> /etc     (루트 밖을 가리키는 링크)
 *   <base>/www/link2 -> ../www2  (이름이 www로 시작하는 옆 폴더)
 *   <base>/www2/x.html
 */
/* realpath()는 POSIX의 XSI 부분이라 _XOPEN_SOURCE를 켜야 선언된다 */
#define _XOPEN_SOURCE 700

#include "file.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static char g_base[] = "/tmp/ws_file_XXXXXX";
static char g_root[PATH_MAX];       /* 테스트 루트의 실제 위치 */

static int str_eq(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs(content, f);
        fclose(f);
    }
}

/* 테스트용 루트를 만든다. 실패하면 0 */
static int make_fixture(void)
{
    char p[PATH_MAX];
    if (mkdtemp(g_base) == NULL)
        return 0;

    snprintf(p, sizeof(p), "%s/www", g_base);          mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/www/sub", g_base);      mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/www/empty", g_base);    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/www2", g_base);         mkdir(p, 0755);

    snprintf(p, sizeof(p), "%s/www/index.html", g_base);     write_file(p, "<h1>hi</h1>\n");
    snprintf(p, sizeof(p), "%s/www/style.css", g_base);      write_file(p, "body {}\n");
    snprintf(p, sizeof(p), "%s/www/sub/index.html", g_base); write_file(p, "sub\n");
    snprintf(p, sizeof(p), "%s/www2/x.html", g_base);        write_file(p, "outside\n");
    snprintf(p, sizeof(p), "%s/www/secret.txt", g_base);     write_file(p, "secret\n");
    chmod(p, 0);

    snprintf(p, sizeof(p), "%s/www/link", g_base);   if (symlink("/etc", p) < 0) return 0;
    snprintf(p, sizeof(p), "%s/www/link2", g_base);  if (symlink("../www2", p) < 0) return 0;

    snprintf(p, sizeof(p), "%s/www", g_base);
    return realpath(p, g_root) != NULL;
}

static void remove_fixture(void)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0)
        printf("  (임시 폴더 %s를 지우지 못함)\n", g_base);
}

/* ---- mime_type ---- */

static void ut01_to_ut11_mime(void)
{
    CHECK("UT-01 .html", str_eq(mime_type("/index.html"), "text/html; charset=utf-8"));
    CHECK("UT-02 .css", str_eq(mime_type("/style.css"), "text/css; charset=utf-8"));
    CHECK("UT-03 .js", str_eq(mime_type("/app.js"), "text/javascript; charset=utf-8"));
    CHECK("UT-04 .png", str_eq(mime_type("/image.png"), "image/png"));
    CHECK("UT-05 .jpg", str_eq(mime_type("/photo.jpg"), "image/jpeg"));
    CHECK("UT-06 .jpeg", str_eq(mime_type("/photo.jpeg"), "image/jpeg"));
    CHECK("UT-07 대문자 .JPG", str_eq(mime_type("/PHOTO.JPG"), "image/jpeg"));
    CHECK("UT-08 모르는 확장자 .bin", str_eq(mime_type("/large.bin"), "application/octet-stream"));
    CHECK("UT-09 확장자 없음", str_eq(mime_type("/README"), "application/octet-stream"));
    CHECK("UT-10 폴더 이름에만 . 이 있음", str_eq(mime_type("/a.d/file"), "application/octet-stream"));
    CHECK("UT-11 마지막 . 기준 (.tar.png)", str_eq(mime_type("/x.tar.png"), "image/png"));
}

/* ---- has_dot_segment ---- */

static void ut12_to_ut19_dot(void)
{
    CHECK("UT-12 /../../etc/passwd → 1", has_dot_segment("/../../etc/passwd") == 1);
    CHECK("UT-13 /a/.. → 1", has_dot_segment("/a/..") == 1);
    CHECK("UT-14 /a/../b → 1", has_dot_segment("/a/../b") == 1);
    CHECK("UT-15 /.. → 1", has_dot_segment("/..") == 1);
    CHECK("UT-16 /a..b.html → 0", has_dot_segment("/a..b.html") == 0);
    CHECK("UT-17 /..hidden → 0", has_dot_segment("/..hidden") == 0);
    CHECK("UT-18 /./style.css → 0", has_dot_segment("/./style.css") == 0);
    CHECK("UT-19 /style.css → 0", has_dot_segment("/style.css") == 0);
}

/* ---- resolve_path ---- */

/* path를 복사해서 resolve_path에 넘기고, 결과 위치가 루트 + want인지 본다 */
static int resolve_is(const char *path, const char *want_rel, int *status)
{
    char p[PATH_MAX], out[PATH_MAX], want[PATH_MAX];
    snprintf(p, sizeof(p), "%s", path);
    *status = resolve_path(g_root, p, out, sizeof(out));
    snprintf(want, sizeof(want), "%s%s", g_root, want_rel);
    return *status == 0 && strcmp(out, want) == 0;
}

static int resolve_status(const char *path)
{
    char p[PATH_MAX], out[PATH_MAX];
    snprintf(p, sizeof(p), "%s", path);
    return resolve_path(g_root, p, out, sizeof(out));
}

static void ut20_to_ut31_resolve(void)
{
    int st;
    CHECK("UT-20 /style.css → 루트/style.css", resolve_is("/style.css", "/style.css", &st));
    CHECK("UT-21 / → 루트/index.html", resolve_is("/", "/index.html", &st));
    CHECK("UT-22 /sub/ → 루트/sub/index.html", resolve_is("/sub/", "/sub/index.html", &st));
    CHECK("UT-23 /sub (끝 / 없음) → 루트/sub/index.html", resolve_is("/sub", "/sub/index.html", &st));
    CHECK("UT-24 /style.css?v=3 → 루트/style.css", resolve_is("/style.css?v=3", "/style.css", &st));

    char p[] = "/style.css?v=3";
    char out[PATH_MAX];
    resolve_path(g_root, p, out, sizeof(out));
    CHECK("UT-24 경로의 ? 자리가 \\0으로 잘림", str_eq(p, "/style.css"));

    CHECK("UT-25 /empty/ (index.html 없음) → 403", resolve_status("/empty/") == 403);
    CHECK("UT-26 /nope.html → 404", resolve_status("/nope.html") == 404);
    CHECK("UT-27 /style.css/x (파일 아래 경로) → 404", resolve_status("/style.css/x") == 404);
    CHECK("UT-28 /../www2/x.html → 403", resolve_status("/../www2/x.html") == 403);
    CHECK("UT-29 /link/passwd (링크로 루트 밖) → 403", resolve_status("/link/passwd") == 403);
    CHECK("UT-30 /link2/x.html (www2로 빠짐) → 403", resolve_status("/link2/x.html") == 403);
    CHECK("UT-31 /secret.txt (읽기 권한 없음)도 위치는 0", resolve_is("/secret.txt", "/secret.txt", &st));
}

int main(void)
{
    printf("test_file\n");
    ut01_to_ut11_mime();
    ut12_to_ut19_dot();
    if (!make_fixture()) {
        printf("  FAIL  테스트 루트를 만들지 못함 (%s)\n", g_base);
        return 1;
    }
    ut20_to_ut31_resolve();
    remove_fixture();
    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
