#define _XOPEN_SOURCE 700 /* realpath는 XSI 함수다. 없으면 암묵 선언으로 64비트 포인터가 잘릴 수 있다(설계 재검토 I1). */

#include "static_file.h"
#include "io.h"
#include "mime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char index_name[] = "/index.html";

/* path[0..len)를 '/'로 나눈 조각 중 정확히 ".."인 것이 있는가(D-21, D-28 3행). */
static int has_dotdot(const char *path, size_t len)
{
    size_t i = 0;

    while (i < len) {
        size_t start = ++i; /* path[i-1]은 '/'다 */
        while (i < len && path[i] != '/') {
            i++;
        }
        if (i - start == 2 && path[start] == '.' && path[start + 1] == '.') {
            return 1;
        }
    }
    return 0;
}

/* realpath·stat 실패의 errno를 결과로 바꾼다(D-28 5행). index 재해석 중이면 없음도 403(7행). */
static static_file_result lookup_error(int err, int in_index)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:
        return in_index ? STATIC_FILE_FORBIDDEN : STATIC_FILE_NOT_FOUND;
    case EACCES:
        return STATIC_FILE_FORBIDDEN;
    case ENAMETOOLONG:
        return STATIC_FILE_BAD_TARGET; /* D-23 */
    default:
        return STATIC_FILE_ERROR; /* errno는 그대로다 */
    }
}

/*
 * D-28 5~6행과 7·8행의 stat: path를 resolved로 정규화하고, 루트 안인지 보고 stat한다.
 * 성공이면 STATIC_FILE_OK. path와 resolved는 모두 PATH_MAX 크기다.
 */
static static_file_result resolve(const char *root, const char *path, char *resolved, struct stat *st,
                                  int in_index)
{
    if (realpath(path, resolved) == NULL) {
        return lookup_error(errno, in_index);
    }
    if (!static_file_within_root(root, resolved)) {
        return STATIC_FILE_FORBIDDEN;
    }
    if (stat(resolved, st) != 0) {
        return lookup_error(errno, in_index);
    }
    return STATIC_FILE_OK;
}

/* 요청 경로 path[0..len)의 연속된 '/'를 하나로 줄이고 끝에 '/'를 붙인다(D-27, C1). */
static void make_location(char *out, const char *path, size_t len)
{
    size_t i, n = 0;

    for (i = 0; i < len; i++) {
        if (path[i] != '/' || n == 0 || out[n - 1] != '/') {
            out[n++] = path[i];
        }
    }
    if (out[n - 1] != '/') {
        out[n++] = '/';
    }
    out[n] = '\0';
}

int static_file_within_root(const char *root, const char *resolved)
{
    size_t n = strlen(root);

    if (strcmp(root, "/") == 0) {
        return resolved[0] == '/';
    }
    return strncmp(resolved, root, n) == 0 && (resolved[n] == '\0' || resolved[n] == '/');
}

static_file_result static_file_open(const char *root, const char *target, static_file *out)
{
    char path[PATH_MAX], resolved[PATH_MAX];
    size_t root_len = strlen(root), path_len, resolved_len;
    const char *content_type;
    static_file_result r;
    struct stat st;
    int fd, err;

    out->fd = -1;
    if (target[0] != '/') { /* 1행 */
        return STATIC_FILE_BAD_TARGET;
    }
    path_len = strcspn(target, "?"); /* 2행 */
    if (has_dotdot(target, path_len)) { /* 3행 */
        return STATIC_FILE_FORBIDDEN;
    }
    if (root_len + path_len >= PATH_MAX) { /* 4행: NUL 포함 상한 */
        return STATIC_FILE_BAD_TARGET;
    }
    memcpy(path, root, root_len);
    memcpy(path + root_len, target, path_len);
    path[root_len + path_len] = '\0';
    content_type = mime_type_for(path); /* D-34: 루트 뒤가 요청 경로이므로 마지막 이름은 요청의 것이다 */

    r = resolve(root, path, resolved, &st, 0); /* 5·6행 */
    if (r != STATIC_FILE_OK) {
        return r;
    }
    if (S_ISDIR(st.st_mode)) { /* 7행 */
        if (target[path_len - 1] != '/') {
            make_location(out->location, target, path_len);
            return STATIC_FILE_REDIRECT;
        }
        resolved_len = strlen(resolved);
        if (resolved_len + sizeof(index_name) > PATH_MAX) { /* sizeof는 NUL을 포함한다 */
            return STATIC_FILE_BAD_TARGET;                 /* D-23, 코드 검토 항목 */
        }
        memcpy(path, resolved, resolved_len);
        memcpy(path + resolved_len, index_name, sizeof(index_name));
        content_type = mime_type_for(index_name);
        r = resolve(root, path, resolved, &st, 1);
        if (r != STATIC_FILE_OK) {
            return r;
        }
        if (S_ISDIR(st.st_mode)) {
            return STATIC_FILE_FORBIDDEN;
        }
    }
    if (!S_ISREG(st.st_mode)) { /* 8행, D-31 */
        return STATIC_FILE_FORBIDDEN;
    }
    /* 9행: O_NONBLOCK은 검사 뒤 FIFO로 바뀌어도 open이 멈추지 않게 한다. 일반 파일 read에는 영향이 없다. */
    fd = open(resolved, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == EACCES) {
            return STATIC_FILE_FORBIDDEN;
        }
        return errno == ENOENT ? STATIC_FILE_NOT_FOUND : STATIC_FILE_ERROR;
    }
    if (fstat(fd, &st) != 0) { /* 10행 */
        err = errno;
        close(fd);
        errno = err;
        return STATIC_FILE_ERROR;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return STATIC_FILE_FORBIDDEN;
    }
    out->fd = fd;
    out->size = (size_t)st.st_size;
    out->content_type = content_type;
    return STATIC_FILE_OK;
}

ssize_t static_file_read_chunk(int fd, void *buf, size_t want)
{
    ssize_t n;

    do {
        n = read(fd, buf, want);
    } while (n < 0 && errno == EINTR);
    if (n == 0) {
        errno = EIO; /* size에 닿기 전에 파일 끝: 전송 중 파일이 줄었다(D-29) */
        return -1;
    }
    return n;
}

int static_file_send_body(int out_fd, const static_file *file)
{
    char buffer[STATIC_FILE_CHUNK];
    size_t left = file->size;

    while (left > 0) {
        ssize_t n = static_file_read_chunk(file->fd, buffer, left < sizeof(buffer) ? left : sizeof(buffer));
        if (n < 0 || io_write_all(out_fd, buffer, (size_t)n) != 0) {
            return -1;
        }
        left -= (size_t)n; /* 파일이 커져도 want 이하만 읽으므로 size까지만 보낸다 */
    }
    return 0;
}
