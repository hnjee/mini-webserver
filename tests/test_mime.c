/* Step 3 단위 테스트: mime_type_for (TC-3-05, D-25 표). */
#include "../src/mime.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    static const struct {
        const char *path;
        const char *type;
    } cases[] = {
        {"index.html", "text/html; charset=utf-8"},
        {"/style.css", "text/css; charset=utf-8"},
        {"/app.js", "text/javascript; charset=utf-8"},
        {"/a/notes.txt", "text/plain; charset=utf-8"},
        {"/image.png", "image/png"},
        {"/photo.jpg", "image/jpeg"},
        {"/photo.jpeg", "image/jpeg"},
        {"/INDEX.HTML", "text/html; charset=utf-8"},
        {"/photo.JPG", "image/jpeg"},
        {"/app.min.js", "text/javascript; charset=utf-8"},
        {"/.txt", "text/plain; charset=utf-8"},
        {"/a.tar.gz", "application/octet-stream"},
        {"/large.bin", "application/octet-stream"},
        {"/README", "application/octet-stream"},
        {"/a.", "application/octet-stream"},
        {"/a.b/c", "application/octet-stream"},
        {"/a.html/", "application/octet-stream"}, /* 마지막 이름이 비었다 */
        {"/htmlx", "application/octet-stream"},
        {"/a.htm", "application/octet-stream"},
        {"/a.xhtml", "application/octet-stream"},
        {"", "application/octet-stream"},
    };
    size_t i;
    int failures = 0;

    printf("test_mime (TC-3-05)\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *got = mime_type_for(cases[i].path);
        if (got != NULL && strcmp(got, cases[i].type) == 0) {
            printf("  ok   TC-3-05 \"%s\" → %s\n", cases[i].path, cases[i].type);
        } else {
            printf("  FAIL TC-3-05 \"%s\" → %s (기대 %s)\n", cases[i].path, got ? got : "(NULL)",
                   cases[i].type);
            failures++;
        }
    }
    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
