#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "http.h"

#define HEADER_BUF_SIZE 8192

/* 연결이 지금 무엇을 하는 중인지 (docs/design/step5.md 상태 전이) */
enum conn_state {
    ST_HEADER,   /* 헤더 읽는 중 */
    ST_BODY,     /* 본문 읽는 중 */
    ST_WRITE     /* 응답 보내는 중 */
};

/* 연결 하나에 하나씩 malloc으로 만들어요 */
struct conn {
    int fd;                       /* 클라이언트 소켓 번호 (논블로킹) */
    char buf[HEADER_BUF_SIZE];    /* 헤더 버퍼 8KB. 요청 사이에도 남은 바이트를 들고 있어요 */
    size_t len;                   /* buf에 지금 들어 있는 바이트 수 */
    struct http_request req;      /* 파싱 결과: buf 안을 가리키는 포인터들 */
    char *body;                   /* 본문 버퍼. Content-Length만큼 malloc, 요청이 끝나면 free */
    size_t body_len;              /* body에 모은 바이트 수 */

    enum conn_state state;        /* 지금 단계 */
    size_t hdr_end;               /* 헤더의 끝 바로 다음 위치 (본문 읽는 중일 때) */
    size_t cl;                    /* 이 요청의 본문 길이 */
    int keep;                     /* 응답을 다 보낸 뒤 연결을 유지할지 */
    time_t last_active;           /* 마지막으로 바이트를 주고받은 시각 (S5-03) */

    char *out;                    /* 보낼 응답 헤더 (에코·오류는 본문까지) */
    size_t out_len;               /* out의 전체 길이 */
    size_t out_sent;              /* out에서 지금까지 보낸 바이트 수 */
    int file_fd;                  /* 보낼 파일. 없으면 -1 */
    off_t file_off;               /* 파일에서 지금까지 보낸 위치 (S5-05) */
    off_t file_size;              /* 파일 크기 */
    int want_out;                 /* EPOLLOUT을 켜 두었으면 1 */

    struct conn *prev, *next;     /* 열린 연결 리스트 */
};

/* 서버 전체 상태 */
struct server {
    int ep;                       /* epoll 인스턴스 fd */
    int listen_fd;                /* 리슨 소켓 (논블로킹) */
    const char *root;             /* 정적 파일 루트의 실제 위치 */
    struct conn *conns;           /* 열린 연결 리스트의 맨 앞 */
    size_t nconn;                 /* 열린 연결 수 */
};

#define MAX_EVENTS 64             /* epoll_wait() 한 번에 받는 최대 개수 */
#define ACCEPT_BATCH 64           /* 리슨 소켓 알림 한 번에 accept하는 최대 개수 (S5-06) */
#define TICK_MS 1000              /* epoll_wait() 타임아웃. 1초마다 유휴 연결 확인 (S5-03) */
#define IDLE_TIMEOUT_SEC 15       /* 이만큼 조용한 연결은 닫아요 */

/* 리슨 소켓을 준비해요. 성공하면 fd, 실패하면 -1 (이유 출력, 소켓 닫음) */
int server_listen(int port);

/* epoll을 만들고, 리슨 소켓을 논블로킹으로 바꿔 등록해요. 성공 0, 실패 -1 */
int server_init(struct server *s, int listen_fd, const char *root);

/* accept()를 EAGAIN이 나오거나 ACCEPT_BATCH개가 될 때까지 반복해요. 꺼낸 연결 수를 돌려줘요 */
int server_on_accept(struct server *s);

/* 읽을 수 있는 만큼 읽고, 요청이 다 모이면 응답을 시작해요. 연결을 닫았으면 c는 이미 해제돼 있어요 */
void conn_on_readable(struct server *s, struct conn *c);

/* 보낼 수 있는 만큼 보내요. 다 보내면 keep에 따라 다음 요청으로 가거나 닫아요 */
void conn_on_writable(struct server *s, struct conn *c);

/* now - last_active >= IDLE_TIMEOUT_SEC인 연결을 닫아요. 닫은 개수를 돌려줘요 */
int server_sweep_idle(struct server *s, time_t now);

/* epoll에서 빼고, 소켓·파일을 닫고, 버퍼와 conn을 해제해요 */
void server_close_conn(struct server *s, struct conn *c);

/* 연결을 모두 닫고 epoll을 닫아요. 리슨 소켓은 닫지 않아요 */
void server_free(struct server *s);

/* server_init 후 epoll_wait 루프를 끝없이 돌아요. root_real은 정적 파일 루트의 실제 위치. 돌아오지 않아요 */
void server_run(int listen_fd, const char *root_real);

#endif
