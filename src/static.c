#include "static.h"

#include "mime.h"
#include "response.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 파일을 이 크기씩 읽어서 보낸다. 파일 크기와 상관없이 메모리는 이만큼만 쓴다 */
#define FILE_CHUNK_SIZE (64 * 1024)

/* real이 root와 같거나 root + "/"로 시작하면 1 ("/app/www2"는 "/app/www" 안이 아니다) */
static int is_inside(const char *root, const char *real)
{
    size_t n = strlen(root);

    if (strcmp(root, "/") == 0)
        return 1;
    return strncmp(real, root, n) == 0 && (real[n] == '\0' || real[n] == '/');
}

/* realpath()나 open()이 실패한 이유(errno)를 상태 코드로 바꾼다 */
static int status_from_errno(int err)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:
        return 404;   /* 없다 */
    case EACCES:
        return 403;   /* 있지만 권한이 없다 */
    default:
        return 500;   /* 서버 쪽 문제 (fd 부족 등) */
    }
}

/*
 * path를 실제 위치로 풀고 검사해 열 수 있는 일반 파일의 경로를 real에 남긴다.
 * 디렉터리면 그 안의 index.html로 한 번 더 검사한다 (심볼릭 링크일 수도 있어서).
 * 정상이면 0, 아니면 응답할 상태 코드를 돌려준다.
 * path는 index.html을 붙일 때 바뀐다 (MIME 판단에 쓴다).
 */
static int resolve_file(const char *root, char *path, char *real)
{
    struct stat st;

    for (int tries = 0; tries < 2; tries++) {
        /* ② 실제 위치: "..", ".", "//", 심볼릭 링크를 모두 푼다 */
        if (realpath(path, real) == NULL)
            return status_from_errno(errno);

        /* ③ 루트 밖으로 나가면 막는다 */
        if (!is_inside(root, real))
            return 403;

        /* ④ 종류 확인 */
        if (stat(real, &st) < 0)
            return status_from_errno(errno);
        if (S_ISREG(st.st_mode))
            return 0;
        if (!S_ISDIR(st.st_mode) || tries == 1)
            return 403;   /* 장치 파일 등, 또는 index.html이 디렉터리 */

        /* 디렉터리면 index.html을 붙여 ②부터 다시 */
        int n = snprintf(path, PATH_MAX, "%s/index.html", real);
        if (n < 0 || n >= PATH_MAX)
            return 414;
    }
    return 403;   /* 도달하지 않는다 */
}

/*
 * 헤더를 보내고 파일을 64KB씩 끝까지 보낸다.
 * 헤더를 보낸 뒤에는 상태 코드를 바꿀 수 없으니, 실패하면 -1만 돌려준다 (호출한 쪽이 닫는다).
 */
static int send_file(int client_fd, int file_fd, const char *content_type, size_t size)
{
    char buf[FILE_CHUNK_SIZE];
    size_t sent = 0;

    if (send_header(client_fd, 200, content_type, size) < 0)
        return -1;

    while (sent < size) {
        ssize_t n = read(file_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;   /* 보내는 중에 파일이 줄었다. Content-Length를 채울 수 없다 */
        if ((size_t)n > size - sent)
            n = (ssize_t)(size - sent);   /* 그 사이 파일이 늘었으면 약속한 크기까지만 */
        if (write_all(client_fd, buf, (size_t)n) < 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

int serve_static(int client_fd, const char *root, const struct http_request *req)
{
    char path[PATH_MAX];
    char real[PATH_MAX];
    struct stat st;
    size_t uri_len;
    int status;
    int file_fd;
    int n;

    /* ① 쿼리 문자열을 떼고, 루트 + URI로 경로를 만든다 */
    uri_len = strcspn(req->uri, "?");
    n = snprintf(path, sizeof(path), "%s%.*s", root, (int)uri_len, req->uri);
    if (n < 0 || n >= (int)sizeof(path)) {
        send_error(client_fd, 414);
        return 414;
    }

    /* ②~④ 실제 위치, 루트 안인지, 종류 */
    status = resolve_file(root, path, real);
    if (status != 0) {
        send_error(client_fd, status);
        return status;
    }

    /* ⑤ 열기: 읽기 권한은 여기서 검사된다 (secret.txt는 여기서 403) */
    file_fd = open(real, O_RDONLY);
    if (file_fd < 0) {
        status = status_from_errno(errno);
        send_error(client_fd, status);
        return status;
    }
    if (fstat(file_fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(file_fd);
        send_error(client_fd, 500);
        return 500;
    }

    /* ⑥ 헤더 + 64KB씩 전송. 실패하면 이미 200을 보냈으니 연결을 닫을 뿐이다 */
    if (send_file(client_fd, file_fd, mime_type(path), (size_t)st.st_size) < 0)
        perror("send_file");
    close(file_fd);
    return 200;
}
