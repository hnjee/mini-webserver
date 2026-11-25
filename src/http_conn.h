#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include "http_parser.h"
#include "http_response.h"
#include "proxy.h"
#include "route.h"

#include <stddef.h>

#define HTTP_CONN_TIMEOUT_NS 5000000000LL /* D-41, D-59: 읽기·쓰기 진행이 없는 5초 */
#define HTTP_CONN_OUT_MAX 65536           /* D-58: 연결별 출력 버퍼 */
#define HTTP_CONN_BACKEND_OUT_MAX 16384   /* D-67: 백엔드로 보낼 버퍼 */

/* D-67 깨운 원인(비트 묶음). 옛 백엔드 소켓의 알림은 루프가 걸러서 넘기지 않는다. */
#define HTTP_CONN_WAKE_INTERNAL 0     /* 내부 실행·만료 뒤 실행 */
#define HTTP_CONN_WAKE_CLIENT 1       /* 클라이언트 소켓 준비(HUP·ERR 포함) */
#define HTTP_CONN_WAKE_BACKEND_IN 2   /* 현재 백엔드 읽기 준비 */
#define HTTP_CONN_WAKE_BACKEND_OUT 4  /* 현재 백엔드 쓰기 준비 */
#define HTTP_CONN_WAKE_BACKEND_ERR 8  /* 현재 백엔드 ERR·HUP */

typedef enum {
    HTTP_CONN_DONE = 0,        /* 요청 사이에 상대가 닫음·유휴 마감, 닫는 응답과 정리 끝 */
    HTTP_CONN_PEER_CLOSED = 1, /* 요청 도중(머리·본문) 상대가 닫았다 */
    HTTP_CONN_TIMED_OUT = 2,   /* 요청 도중 읽기 마감 */
    HTTP_CONN_ERROR = -1       /* 읽기·쓰기 실패, 쓰기 마감(EAGAIN). err에 오류 번호 */
} http_conn_result;

/* 한 차례의 결과(D-57). 루프는 이것만 보고 관심 방향·내부 실행 목록·정리를 정한다. */
typedef enum {
    HTTP_CONN_WANT_READ,  /* 소켓 읽기 준비를 기다린다 */
    HTTP_CONN_WANT_WRITE, /* 소켓 쓰기 준비를 기다린다 */
    HTTP_CONN_RUNNABLE,   /* 기다릴 것 없이 다음 회차에 다시 부른다(D-56 내부 실행) */
    HTTP_CONN_FINISHED,   /* 끝. result·err가 정해졌다. 다시 부르지 않는다 */
    HTTP_CONN_WANT_BACKEND_READ, /* 백엔드 읽기 준비를 기다린다(D-67) */
    HTTP_CONN_WANT_BACKEND_WRITE /* 백엔드 쓰기 준비(연결 완료 포함)를 기다린다(D-67) */
} http_conn_step;

/*
 * 연결 하나의 상태(D-57). 필드는 http_conn.c가 바꾼다(예외: 관심 방향 변경 실패 때 루프가 result·err를 쓴다).
 * 메모리는 루프(또는 시험)가 가진다.
 * 워커 확장: http_conn_* 함수는 워커 스레드에서도 불린다. 한 연결은 한 번에 한 스레드만 접근한다
 * (워커가 실행하는 동안 루프는 이 구조체를 읽지도 쓰지도 않는다).
 * 시간은 모두 CLOCK_MONOTONIC 나노초이고 호출자가 now로 넘긴다.
 */
typedef struct {
    int fd;           /* 빌린 소켓. 닫지 않는다 */
    const char *root; /* main이 정규화한 루트(빌림) */
    route_table *routes; /* main이 빌려준 라우팅 표, 없으면 NULL(D-61). 라운드 로빈 카운터만 바꾼다 */
    int state;        /* http_conn.c의 상태 값 */
    int wait;         /* 기다리는 종류(D-59 표의 행) */
    long long deadline;
    http_conn_result result;
    int err;

    char in[HTTP_REQUEST_HEAD_MAX]; /* D-47: 맨 앞은 아직 처리하지 않은 요청의 시작이다 */
    size_t in_len;

    http_request req;         /* 처리 중인 요청 정보 */
    http_connection conn_hdr; /* 응답 Connection 머리(D-51) */
    int close_after;          /* 0 유지, 1 닫기, 2 shutdown 뒤 정리(D-54) */
    int echo;                 /* POST /echo인가 */
    size_t body_buffered;     /* in 안의 본문 바이트(머리 바로 뒤) */
    size_t body_left;         /* 소켓에서 더 읽을 본문 바이트 */

    int file_fd; /* 보내는 중인 파일, 없으면 -1 */
    size_t file_left;

    char out[HTTP_CONN_OUT_MAX]; /* D-58: 다 보낸 뒤에만 다시 채운다 */
    size_t out_pos, out_len;

    /* Step 6 프록시(D-64~D-67). 루프는 be_fd·be_gen·uploading만 읽는다 */
    int be_fd;          /* 백엔드 소켓, 없으면 -1. http_conn이 열고 닫는다 */
    unsigned be_gen;    /* 백엔드 소켓을 열거나 닫을 때마다 늘어난다(옛 알림 구분) */
    int uploading;      /* 요청을 백엔드로 보내는 중: 루프가 백엔드 읽기도 등록한다 */
    int committed;      /* 응답 머리를 출력 버퍼에 채웠다(그 뒤에는 502·504 불가) */
    int pending_status; /* 본문을 다 버린 뒤 보낼 502·504, 없으면 0 */
    proxy_body resp_body;
    size_t resp_left; /* PROXY_BODY_LENGTH: 백엔드에서 더 읽을 본문 바이트 */
    char be_out[HTTP_CONN_BACKEND_OUT_MAX];
    size_t be_out_pos, be_out_len;
    char be_in[PROXY_RESPONSE_HEAD_MAX]; /* 응답 머리 모으기 */
    size_t be_in_len;
} http_conn;

/* 새 연결(fd는 논블로킹). 요청 사이 대기로 시작하고 마감은 now + 5초다. routes는 NULL이면 프록시 없음. */
void http_conn_init(http_conn *c, int fd, const char *root, route_table *routes, long long now);

/*
 * 한 차례를 실행한다(D-56 예산: 소켓 read 1번(백엔드 read 포함), 파일 조각 1번, 백엔드 write 1번,
 * 차례 시작 때 있던 출력까지 write). wake는 HTTP_CONN_WAKE_* 묶음이다(D-67).
 * 준비 알림(HUP·ERR 포함)이 오거나 RUNNABLE이었으면 부른다. FINISHED 뒤에는 부르지 않는다.
 * WANT_READ인데 uploading이면 루프는 백엔드 읽기도, WANT_BACKEND_WRITE인데 uploading이면 백엔드 읽기·쓰기를 등록한다.
 */
http_conn_step http_conn_run(http_conn *c, int wake, long long now);

/* 기다리는 중인 연결의 마감이 지났는가(D-59). RUNNABLE·FINISHED 뒤에는 묻지 않는다. */
int http_conn_expired(const http_conn *c, long long now);

/*
 * 마감 처리. 기다리던 종류에 따라 DONE·TIMED_OUT·ERROR(EAGAIN·ETIMEDOUT)를 기록하고 FINISHED,
 * 커밋 전 백엔드 대기면 백엔드를 닫고 504 순서로 넘어가 RUNNABLE을 돌려준다(D-67).
 */
http_conn_step http_conn_expire(http_conn *c);

/* 연결 자원 해제: 열려 있는 파일 fd와 백엔드 소켓을 닫는다. 클라이언트 소켓은 닫지 않는다(루프가 닫는다). */
void http_conn_release(http_conn *c);

#endif /* HTTP_CONN_H */
