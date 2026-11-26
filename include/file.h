#ifndef FILE_H
#define FILE_H

#include <stddef.h>

/* 확장자로 Content-Type 값을 돌려줘요. 모르거나 없으면 "application/octet-stream" */
const char *mime_type(const char *path);

/* 경로를 /로 나눈 조각 중에 정확히 ".."인 것이 있으면 1, 없으면 0 */
int has_dot_segment(const char *path);

/* 요청 경로를 루트 안의 실제 파일 위치로 바꿔 out(PATH_MAX 이상)에 넣어요.
   path의 '?' 자리에 '\0'을 써요. 성공 0, 실패하면 403·404·500 */
int resolve_path(const char *root_real, char *path, char *out, size_t cap);

#endif
