/*
 * 정적 파일 경로와 MIME: docs/design/step3.md
 * 요청 경로를 루트 안의 실제 파일 위치로 바꾼다. 파일을 열지는 않는다.
 */
/* realpath()는 POSIX의 XSI 부분이라 _XOPEN_SOURCE를 켜야 선언된다 */
#define _XOPEN_SOURCE 700

#include "file.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    {"html", "text/html; charset=utf-8"},
    {"css",  "text/css; charset=utf-8"},
    {"js",   "text/javascript; charset=utf-8"},
    {"txt",  "text/plain; charset=utf-8"},
    {"png",  "image/png"},
    {"jpg",  "image/jpeg"},
    {"jpeg", "image/jpeg"},
};

const char *mime_type(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (dot == NULL || (slash != NULL && dot < slash))
        return "application/octet-stream";

    for (size_t i = 0; i < sizeof(mime_table) / sizeof(mime_table[0]); i++) {
        if (strcasecmp(dot + 1, mime_table[i].ext) == 0)
            return mime_table[i].type;
    }
    return "application/octet-stream";
}

int has_dot_segment(const char *path)
{
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.')
            return 1;
        p += n;
    }
    return 0;
}

/* realpath()·stat() 실패 이유를 상태 코드로 */
static int errno_to_status(int err)
{
    if (err == ENOENT || err == ENOTDIR)
        return 404;
    if (err == EACCES)
        return 403;
    return 500;
}

/* p가 루트 자체이거나 "루트/"로 시작하면 1 */
static int is_inside(const char *root_real, const char *p)
{
    size_t n = strlen(root_real);
    return strncmp(p, root_real, n) == 0 && (p[n] == '\0' || p[n] == '/');
}

int resolve_path(const char *root_real, char *path, char *out, size_t cap)
{
    char joined[PATH_MAX];
    struct stat st;

    char *q = strchr(path, '?');
    if (q != NULL)
        *q = '\0';                               /* 쿼리 문자열 자르기 */

    if (has_dot_segment(path))
        return 403;

    if (cap < PATH_MAX)
        return 500;                              /* realpath()는 out에 PATH_MAX까지 써요 */
    if ((size_t)snprintf(joined, sizeof(joined), "%s%s", root_real, path) >= sizeof(joined))
        return 404;
    if (realpath(joined, out) == NULL)
        return errno_to_status(errno);
    if (!is_inside(root_real, out))
        return 403;

    if (stat(out, &st) < 0)
        return errno_to_status(errno);
    if (S_ISDIR(st.st_mode)) {
        if ((size_t)snprintf(joined, sizeof(joined), "%s/index.html", out) >= sizeof(joined))
            return 404;
        if (realpath(joined, out) == NULL)
            return errno == ENOENT ? 403 : errno_to_status(errno);   /* index.html 없음 → 403 */
        if (!is_inside(root_real, out))
            return 403;
    }
    return 0;
}
