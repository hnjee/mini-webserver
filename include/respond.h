#ifndef RESPOND_H
#define RESPOND_H

#include "server.h"

/* 응답을 바로 보내지 않고 c->out(과 정적 파일이면 c->file_fd)에 준비해요. conn.c가 보내요. */

/* 오류 응답. 본문은 "코드 문구\n", extra는 "Allow: GET" 같은 헤더 한 줄이거나 NULL. 성공 0, 메모리 부족 -1 */
int prepare_error(struct conn *c, int status, const char *extra);

/* 경로와 메서드에 맞는 응답(에코, 405, 정적 파일, 403·404·500)을 준비해요. 성공 0, 메모리 부족 -1 */
int prepare_response(struct server *s, struct conn *c);

/* 이번 요청의 응답 자원(out, 파일, 본문)을 정리해요 */
void response_reset(struct conn *c);

#endif
