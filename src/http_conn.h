#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include "http_parser.h"
#include "http_response.h"

#include <stddef.h>

#define HTTP_CONN_TIMEOUT_NS 5000000000LL /* D-41, D-59: 읽기·쓰기 진행이 없는 5초 */
#define HTTP_CONN_OUT_MAX 65536           /* D-58: 연결별 출력 버퍼 */

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
    HTTP_CONN_FINISHED    /* 끝. result·err가 정해졌다. 다시 부르지 않는다 */
} http_conn_step;

/*
 * 연결 하나의 상태(D-57). 필드는 http_conn.c가 바꾼다(예외: 관심 방향 변경 실패 때 루프가 result·err를 쓴다).
 * 메모리는 루프(또는 시험)가 가진다.
 * 시간은 모두 CLOCK_MONOTONIC 나노초이고 호출자가 now로 넘긴다.
 */
typedef struct {
    int fd;           /* 빌린 소켓. 닫지 않는다 */
    const char *root; /* main이 정규화한 루트(빌림) */
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
} http_conn;

/* 새 연결(fd는 논블로킹). 요청 사이 대기로 시작하고 마감은 now + 5초다. */
void http_conn_init(http_conn *c, int fd, const char *root, long long now);

/*
 * 한 차례를 실행한다(D-56 예산: 소켓 read 1번, 파일 조각 1번, 차례 시작 때 있던 출력까지 write).
 * 준비 알림(HUP·ERR 포함)이 오거나 RUNNABLE이었으면 부른다. FINISHED 뒤에는 부르지 않는다.
 */
http_conn_step http_conn_run(http_conn *c, long long now);

/* 기다리는 중인 연결의 마감이 지났는가(D-59). RUNNABLE·FINISHED 뒤에는 묻지 않는다. */
int http_conn_expired(const http_conn *c, long long now);

/* 마감으로 끝낸다. 기다리던 종류에 따라 DONE·TIMED_OUT·ERROR(EAGAIN)를 기록한다. */
void http_conn_expire(http_conn *c);

/* 연결 자원 해제: 열려 있는 파일 fd를 닫는다. 소켓은 닫지 않는다(루프가 닫는다). */
void http_conn_release(http_conn *c);

#endif /* HTTP_CONN_H */
