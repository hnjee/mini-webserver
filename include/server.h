#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

#include "http.h"

#define HEADER_BUF_SIZE 8192

/* 연결 하나에 하나씩 malloc으로 만들어요 */
struct conn {
    int fd;                       /* 클라이언트 소켓 번호 */
    char buf[HEADER_BUF_SIZE];    /* 요청 줄과 헤더를 모으는 헤더 버퍼 */
    size_t len;                   /* buf에 지금까지 모은 바이트 수 */
    struct http_request req;      /* 파싱 결과: buf 안을 가리키는 포인터들 */
};

/* 리슨 소켓을 준비해요. 성공하면 fd, 실패하면 -1 (이유 출력, 소켓 닫음) */
int server_listen(int port);

/* accept → handle_conn → close를 끝없이 반복해요. root_real은 정적 파일 루트의 실제 위치. 돌아오지 않아요. */
void server_run(int listen_fd, const char *root_real);

/* 요청 하나를 읽고, root_real 아래 파일이나 오류 응답 하나를 보낸 뒤 남은 데이터를 비워요.
   응답했으면 0, 응답 없이 끝나거나 보내다 끊기면 -1. 소켓은 닫지 않아요. */
int handle_conn(struct conn *c, const char *root_real);

/* len바이트를 다 쓸 때까지 write()를 반복해요. 성공 0, 실패 -1 */
int write_all(int fd, const char *buf, size_t len);

#endif
