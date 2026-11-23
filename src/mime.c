#define _POSIX_C_SOURCE 200809L

#include "mime.h"

#include <string.h>
#include <strings.h>

const char *mime_type_for(const char *path)
{
    static const struct {
        const char *ext;
        const char *type;
    } table[] = {
        {"html", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"}, /* RFC 9239 */
        {"txt", "text/plain; charset=utf-8"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
    };
    const char *name = strrchr(path, '/');
    const char *dot;
    size_t i;

    name = name != NULL ? name + 1 : path;
    dot = strrchr(name, '.');
    if (dot != NULL) {
        for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            if (strcasecmp(dot + 1, table[i].ext) == 0) {
                return table[i].type;
            }
        }
    }
    return "application/octet-stream";
}
