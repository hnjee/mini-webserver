#ifndef STATIC_H
#define STATIC_H

#include "request.h"

/*
 * req->uri가 가리키는 root 안의 파일을 client_fd로 보낸다.
 * root는 realpath()로 구한 절대 경로여야 한다 (서버 시작할 때 한 번 구한다).
 *   - '?' 뒤의 쿼리 문자열은 뗀다. %XX는 디코딩하지 않는다
 *   - 디렉터리면 그 안의 index.html을 보낸다
 *   - 없으면 404, 루트 밖이거나 읽을 수 없으면 403, 경로가 너무 길면 414,
 *     그 밖의 서버 쪽 실패는 500으로 응답한다
 * 보낸 응답의 상태 코드를 돌려준다 (로그용).
 */
int serve_static(int client_fd, const char *root, const struct http_request *req);

#endif
