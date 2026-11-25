#ifndef LOG_H
#define LOG_H

#include "server.h"

/* 접근 로그를 쓸 fd. 기본은 -1(끔). main.c가 STDOUT_FILENO로 켜요 (S7-06) */
void access_log_set_fd(int fd);

/* 응답 하나를 한 줄로 남겨요. write() 한 번으로 써서 여러 워커의 줄이 섞이지 않아요.
   형식: 2026-11-26 00:20:01 127.0.0.1 "GET /x HTTP/1.1" 200 1234 3ms
   요청 줄을 못 읽었으면 "-". 남긴 뒤 c->log_pending, status, bytes_out을 비워요 */
void access_log_conn(struct conn *c);

#endif
