#ifndef STATIC_FILE_H
#define STATIC_FILE_H

/* PATH_MAX는 기능 매크로(_POSIX_C_SOURCE 등)가 있어야 정의된다. 포함하는 파일은 그 매크로를 먼저 둔다(설계 재검토 I1). */
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#define STATIC_FILE_CHUNK 65536 /* D-22 */

typedef enum {
    STATIC_FILE_OK,         /* 200: fd, size, content_type 유효 */
    STATIC_FILE_REDIRECT,   /* 301: location 유효 */
    STATIC_FILE_BAD_TARGET, /* 400: '/'로 시작하지 않는다, 경로가 너무 길다(D-23) */
    STATIC_FILE_FORBIDDEN,  /* 403 */
    STATIC_FILE_NOT_FOUND,  /* 404 */
    STATIC_FILE_ERROR       /* 500: errno 보존 */
} static_file_result;

typedef struct {
    int fd;                      /* OK에서만 유효. 호출자가 닫는다 */
    size_t size;                 /* OK: fstat 크기 */
    const char *content_type;    /* OK: mime_type_for 결과(정적 문자열) */
    char location[PATH_MAX + 2]; /* REDIRECT: 연속 '/'를 줄인 경로 + "/" + NUL */
} static_file;

/*
 * D-28 판정 순서대로 요청 대상을 분류하고, OK면 파일을 연다.
 * root: main이 realpath로 정규화한 루트. target: 파서가 돌려준 요청 대상(쿼리 포함).
 * OK가 아닌 결과에서는 이 함수가 연 fd가 남지 않는다(D-29).
 */
static_file_result static_file_open(const char *root, const char *target, static_file *out);

/* resolved가 root와 같거나 root + "/"로 시작하면 1. root가 "/"이면 절대 경로 전부 1 (D-21, D-28 6행). */
int static_file_within_root(const char *root, const char *resolved);

/*
 * file->fd에서 file->size 바이트를 STATIC_FILE_CHUNK씩 read해 io_write_all(out_fd, ...)로 쓴다(D-29).
 * 성공 0, 실패 -1(errno 보존. size에 닿기 전에 파일 끝이면 errno = EIO). fd는 둘 다 닫지 않는다.
 */
int static_file_send_body(int out_fd, const static_file *file);

/*
 * D-58: 파일 fd에서 want(> 0)바이트 이하를 한 번 읽는다(EINTR은 다시 읽는다). 읽은 바이트 수를 돌려준다.
 * 호출자는 보낼 길이가 남았을 때만 부르므로 파일 끝이면 -1(errno = EIO, D-29). 읽기 실패도 -1(errno 보존).
 */
ssize_t static_file_read_chunk(int fd, void *buf, size_t want);

#endif /* STATIC_FILE_H */
