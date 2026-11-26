/*
 * 응답 만들기: docs/design/step3.md ~ step5.md
 * 경로와 메서드를 보고 에코, 오류, 정적 파일 응답을 c->out에 준비해요. 보내기는 conn.c가 해요.
 */
#include "respond.h"

#include "file.h"
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* path가 name과 같은지. ? 뒤 쿼리 문자열은 무시해요 */
static int path_is(const char *path, const char *name)
{
    size_t n = strlen(name);
    return strncmp(path, name, n) == 0 && (path[n] == '\0' || path[n] == '?');
}

/* 이번 요청의 응답 자원(out, 파일, 본문)을 정리해요 */
void response_reset(struct conn *c)
{
    free(c->out);
    c->out = NULL;
    c->out_len = c->out_sent = 0;
    if (c->file_fd >= 0)
        close(c->file_fd);
    c->file_fd = -1;
    c->file_off = c->file_size = 0;
    free(c->body);
    c->body = NULL;
    c->body_len = 0;
}

/* 오류 응답. 본문은 "코드 문구\n", extra는 "Allow: GET" 같은 헤더 한 줄이거나 NULL */
int prepare_error(struct conn *c, int status, const char *extra)
{
    char body[64];
    int body_len = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
    c->out = malloc(512);
    if (c->out == NULL)
        return -1;
    int n = snprintf(c->out, 512,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %d\r\n"
                     "%s%s"
                     "Connection: %s\r\n"
                     "\r\n"
                     "%s",
                     status, http_reason(status), body_len,
                     extra ? extra : "", extra ? "\r\n" : "",
                     c->keep ? "keep-alive" : "close", body);
    c->out_len = (size_t)n;
    return 0;
}

/* 에코: 헤더와 받은 본문을 out에 한 번에 담아요 (S4-07) */
static int prepare_echo(struct conn *c)
{
    const char *type = http_find_header(&c->req, "Content-Type");
    char head[HEADER_BUF_SIZE];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     type != NULL ? type : "application/octet-stream",
                     c->body_len, c->keep ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= sizeof(head))
        return prepare_error(c, 500, NULL);
    c->out = malloc((size_t)n + c->body_len);
    if (c->out == NULL)
        return -1;
    memcpy(c->out, head, (size_t)n);
    if (c->body_len > 0)
        memcpy(c->out + n, c->body, c->body_len);
    c->out_len = (size_t)n + c->body_len;
    return 0;
}

/* 정적 파일: 파일을 열어 들고 200 헤더만 out에 써요. 본문은 flush()가 조금씩 보내요.
   0: 준비됨, 403·404·500: 오류 응답을 보낼 것 */
static int prepare_file(struct conn *c, const char *file)
{
    int ffd = open(file, O_RDONLY);
    if (ffd < 0)
        return errno == ENOENT ? 404 : errno == EACCES ? 403 : 500;
    struct stat st;
    if (fstat(ffd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(ffd);
        return 403;
    }
    c->out = malloc(256);
    if (c->out == NULL) {
        close(ffd);
        return 500;
    }
    int n = snprintf(c->out, 256,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     mime_type(file), (long long)st.st_size, c->keep ? "keep-alive" : "close");
    c->out_len = (size_t)n;
    c->file_fd = ffd;
    c->file_off = 0;
    c->file_size = st.st_size;
    return 0;
}

/* 경로와 메서드에 맞는 응답을 준비해요. 성공 0, 메모리 부족 -1 */
int prepare_response(struct server *s, struct conn *c)
{
    int post = strcmp(c->req.method, "POST") == 0;
    int r;
    if (path_is(c->req.path, "/echo"))
        r = post ? prepare_echo(c) : prepare_error(c, 405, "Allow: POST");
    else if (post)
        r = prepare_error(c, 405, "Allow: GET");
    else {
        char file[PATH_MAX];
        int status = resolve_path(s->root, c->req.path, file, sizeof(file));
        if (status == 0)
            status = prepare_file(c, file);
        r = status == 0 ? 0 : prepare_error(c, status, NULL);
    }
    free(c->body);                               /* 에코는 out에 복사해 뒀어요 */
    c->body = NULL;
    c->body_len = 0;
    return r;
}
