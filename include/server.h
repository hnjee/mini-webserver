#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

#include "http.h"

#define HEADER_BUF_SIZE 8192

/* 연결 하나에 하나씩 malloc으로 만들어요 */
struct conn {
    int fd;                       /* 클라이언트 소켓 번호 */
    char buf[HEADER_BUF_SIZE];    /* 헤더 버퍼 8KB. 요청 사이에도 남은 바이트를 들고 있어요 */
    size_t len;                   /* buf에 지금 들어 있는 바이트 수 */
    struct http_request req;      /* 파싱 결과: buf 안을 가리키는 포인터들 */
    char *body;                   /* 본문 버퍼. Content-Length만큼 malloc, 요청이 끝나면 free */
    size_t body_len;              /* body에 모은 바이트 수 */
};

/* 리슨 소켓을 준비해요. 성공하면 fd, 실패하면 -1 (이유 출력, 소켓 닫음) */
int server_listen(int port);

/* accept → handle_conn → close를 끝없이 반복해요. root_real은 정적 파일 루트의 실제 위치. 돌아오지 않아요. */
void server_run(int listen_fd, const char *root_real);

/* 연결이 끝날 때까지 요청을 읽고 응답하기를 반복해요 (keep-alive).
   요청 사이에서 정상으로 끝나면 0, 요청 중간에 끊기거나 보내다 끊기면 -1. 소켓은 닫지 않아요. */
int handle_conn(struct conn *c, const char *root_real);

/* len바이트를 다 쓸 때까지 write()를 반복해요. 성공 0, 실패 -1 */
int write_all(int fd, const char *buf, size_t len);

#endif
