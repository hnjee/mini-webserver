#include "mime.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

#define MIME_DEFAULT "application/octet-stream"

/* 새 타입은 이 표에 한 줄만 추가하면 된다 */
static const struct {
    const char *ext;
    const char *type;
} MIME_TABLE[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "text/javascript; charset=utf-8" },
    { "txt",  "text/plain; charset=utf-8" },
    { "json", "application/json" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "svg",  "image/svg+xml" },
    { "ico",  "image/x-icon" },
};

const char *mime_type(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;   /* 디렉터리 이름의 '.'은 보지 않는다 */
    const char *dot = strrchr(name, '.');

    /* 점이 없거나, 이름이 점으로 시작하는 숨김 파일(".env")이면 확장자가 없다 */
    if (dot == NULL || dot == name)
        return MIME_DEFAULT;

    for (size_t i = 0; i < sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]); i++) {
        if (strcasecmp(dot + 1, MIME_TABLE[i].ext) == 0)
            return MIME_TABLE[i].type;
    }
    return MIME_DEFAULT;
}
