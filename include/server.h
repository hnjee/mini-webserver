#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "http.h"

struct routes;

#define HEADER_BUF_SIZE 8192

/* 연결이 지금 무엇을 하는 중인지 (docs/design/step5.md, step6.md 상태) */
enum conn_state {
    ST_HEADER,          /* 헤더 읽는 중 */
    ST_BODY,            /* 본문 읽는 중 */
    ST_WRITE,           /* 응답 보내는 중 */
    ST_PROXY_CONNECT,   /* 백엔드 연결 기다리는 중 (Step 6) */
    ST_PROXY_SEND,      /* 백엔드에 요청 보내는 중 */
    ST_PROXY_RELAY      /* 백엔드 응답 중계하는 중 */
};

/* epoll에 맡기는 손잡이. 알림이 리슨 소켓, 클라이언트 소켓, 백엔드 소켓 중 어디서 왔는지 알려 줘요 (step6.md 3장) */
enum handle_kind { H_LISTEN, H_CLIENT, H_BACKEND, H_STOP };

struct conn;

struct handle {
    enum handle_kind kind;
    struct conn *c;               /* H_LISTEN이면 NULL */
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

    /* ---- 프록시 (Step 6) ---- */
    struct handle h_cli, h_back;  /* epoll에 맡기는 손잡이 */
    int back_fd;                  /* 백엔드 소켓. 없으면 -1 */
    char *relay;                  /* 중계 버퍼 PROXY_RELAY_SIZE. 프록시 중에만 있음 */
    size_t rpos, wpos;            /* 중계 버퍼에서 보낸 위치, 채운 위치 */
    size_t sent_to_client;        /* 클라이언트에 넘긴 바이트 수. 0이면 아직 오류 응답을 보낼 수 있음 */
    int back_eof;                 /* 백엔드가 닫았으면 1 */
    time_t deadline;              /* 이 시각이 지나면 504 또는 닫기 */
    unsigned cli_events, back_events; /* 지금 epoll에 걸어 둔 일 */
    int cli_halfclosed;           /* 클라이언트가 쓰기만 닫았으면 1 (EPOLLRDHUP을 더는 안 봄) */

    /* ---- 안정성과 접근 로그 (Step 7) ---- */
    time_t req_start;             /* 이 요청의 첫 바이트가 온 시각. 헤더 30초 상한에 써요 (S7-04) */
    char ip[16];                  /* 클라이언트 IPv4 주소 (로그용) */
    int status;                   /* 보내는 응답의 상태 코드. 0이면 아직 모름 */
    size_t bytes_out;             /* 이번 응답에서 클라이언트에 보낸 바이트 수 */
    struct timespec t_req;        /* 요청 헤더를 다 받은 시각 (처리 시간용) */
    int has_req;                  /* 요청 줄을 파싱했으면 1 (로그에 요청 줄을 남김) */
    int log_pending;              /* 아직 로그를 안 남긴 응답이 있으면 1 */

    int closed;                   /* 닫혔으면 1. 이번 바퀴가 끝나면 해제해요 */
    struct conn *prev, *next;     /* 열린 연결 리스트 (닫히면 해제 대기 리스트) */
};

/* 서버 전체 상태 */
struct server {
    int ep;                       /* epoll 인스턴스 fd */
    int listen_fd;                /* 리슨 소켓 (논블로킹) */
    const char *root;             /* 정적 파일 루트의 실제 위치 */
    struct conn *conns;           /* 열린 연결 리스트의 맨 앞 */
    size_t nconn;                 /* 열린 연결 수 */
    struct conn *dead;            /* 이번 바퀴에 닫힌 연결. 바퀴가 끝나면 해제 */
    struct handle h_listen;       /* 리슨 소켓 손잡이 */
    struct routes *routes;        /* 라우팅 표. NULL이면 프록시 없음 (server_init 뒤에 넣어요) */
    int connect_timeout;          /* 백엔드 연결 시간 제한(초), 기본 PROXY_CONNECT_TIMEOUT_SEC */
    int response_timeout;         /* 백엔드 응답 시간 제한(초), 기본 PROXY_RESPONSE_TIMEOUT_SEC */

    /* ---- 종료와 accept 쉬기 (Step 7) ---- */
    int stop_fd;                  /* 종료 알림 eventfd. 없으면 -1 */
    struct handle h_stop;         /* 종료 알림 손잡이 */
    int stopping;                 /* 종료 중이면 1: 새 연결 안 받고, 응답 뒤에 닫아요 */
    time_t stop_at;               /* 종료를 시작한 시각 */
    int accept_paused;            /* fd가 바닥나 리슨 소켓 감시를 쉬는 중이면 1 (S7-05) */
    time_t accept_resume;         /* 이 시각부터 다시 받아요 */
};

#define MAX_EVENTS 64             /* epoll_wait() 한 번에 받는 최대 개수 */
#define ACCEPT_BATCH 64           /* 리슨 소켓 알림 한 번에 accept하는 최대 개수 (S5-06) */
#define TICK_MS 1000              /* epoll_wait() 타임아웃. 1초마다 유휴 연결 확인 (S5-03) */
#define IDLE_TIMEOUT_SEC 15       /* 이만큼 조용한 연결은 닫아요 */
#define PROXY_RELAY_SIZE 65536    /* 중계 버퍼 64KB (S6-04) */
#define PROXY_CONNECT_TIMEOUT_SEC 3   /* 백엔드 연결 시간 제한 (S6-07) */
#define PROXY_RESPONSE_TIMEOUT_SEC 10 /* 백엔드 응답 시간 제한 (S6-07) */
#define WORKERS_MAX 256           /* -t 상한 (S6-01) */
#define HEADER_TIMEOUT_SEC 30     /* 요청 헤더를 이 안에 다 못 받으면 408 (S7-04) */
#define SHUTDOWN_GRACE_SEC 5      /* 종료 신호 뒤 하던 응답을 기다리는 최대 시간 (S7-02) */
#define ACCEPT_PAUSE_SEC 1        /* fd가 바닥나면 리슨 소켓 감시를 쉬는 시간 (S7-05) */

/* 리슨 소켓을 준비해요. 성공하면 fd, 실패하면 -1 (이유 출력, 소켓 닫음) */
int server_listen(int port);

/* epoll을 만들고, 리슨 소켓을 논블로킹으로 바꿔 EPOLLIN | EPOLLEXCLUSIVE로 등록해요. 성공 0, 실패 -1 */
int server_init(struct server *s, int listen_fd, const char *root);

/* accept()를 EAGAIN이 나오거나 ACCEPT_BATCH개가 될 때까지 반복해요. 꺼낸 연결 수를 돌려줘요 */
int server_on_accept(struct server *s);

/* 읽을 수 있는 만큼 읽고, 요청이 다 모이면 응답을 시작해요. 연결을 닫았으면 c는 이미 해제돼 있어요 */
void conn_on_readable(struct server *s, struct conn *c);

/* 보낼 수 있는 만큼 보내요. 다 보내면 keep에 따라 다음 요청으로 가거나 닫아요 */
void conn_on_writable(struct server *s, struct conn *c);

/* 클라이언트 소켓이 지켜볼 일을 바꿔요. 지금과 같으면 epoll_ctl을 생략해요 */
void conn_watch_client(struct server *s, struct conn *c, unsigned events);

/* 남은 응답 자원을 정리하고 오류 응답(502·504 등)을 보낸 뒤 연결을 닫아요 (keep = 0) */
void conn_send_error(struct server *s, struct conn *c, int status);

/* now - last_active >= IDLE_TIMEOUT_SEC인 연결을 닫아요. 프록시 중인 연결은 deadline을 보고 504를 보내거나 닫아요.
   닫은(또는 504로 넘긴) 개수를 돌려줘요 */
int server_sweep_idle(struct server *s, time_t now);

/* epoll에서 빼고, 소켓(백엔드 포함)·파일을 닫고, 버퍼를 해제해요. conn은 이번 바퀴가 끝날 때 해제돼요 */
void server_close_conn(struct server *s, struct conn *c);

/* 연결을 모두 닫고 epoll을 닫아요. 리슨 소켓은 닫지 않아요 */
void server_free(struct server *s);

/* 종료 알림 eventfd를 epoll에 등록해요. 읽을 수 있게 되면 server_begin_stop()을 불러요. 성공 0, 실패 -1 */
int server_watch_stop(struct server *s, int stop_fd);

/* 종료를 시작해요: 리슨 소켓 감시를 끊고, 요청을 기다리며 쉬는 연결을 닫아요.
   하던 요청은 응답까지 보낸 뒤 닫아요 (keep-alive 안 함). 닫은 연결 수를 돌려줘요 (S7-02) */
int server_begin_stop(struct server *s, time_t now);

/* 워커가 루프를 끝내도 되면 1: 종료 중이고, 연결이 다 닫혔거나 SHUTDOWN_GRACE_SEC가 지났을 때 */
int server_should_exit(const struct server *s, time_t now);

/* epoll_wait()를 한 번 부르고 알림을 처리해요. 닫힌 conn도 여기서 해제해요. 처리한 알림 수, 실패하면 -1 */
int server_poll_once(struct server *s, int timeout_ms);

/* 워커 스레드 nworkers개를 띄워 각자 server_init 후 epoll 루프를 돌려요.
   메인 스레드만 SIGINT·SIGTERM을 받아요. 한 번 오면 워커들에 종료를 알리고, 다 끝나면 돌아와요.
   기다리는 중에 한 번 더 오면 그 자리에서 _exit(130)해요 (S7-02, S7-03).
   root_real은 정적 파일 루트의 실제 위치, routes는 NULL이면 프록시 없음 */
void server_run(int listen_fd, const char *root_real, struct routes *routes, int nworkers);

#endif
