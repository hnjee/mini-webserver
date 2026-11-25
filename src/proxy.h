#ifndef PROXY_H
#define PROXY_H

#include "http_parser.h"
#include "http_response.h"

#include <stddef.h>

/*
 * D-64·D-65: 백엔드 요청 머리 만들기, 백엔드 응답 머리 검증, 클라이언트로 보낼 응답 머리 다시 쓰기.
 * I/O·할당·전역 상태가 없는 순수 함수다. 연결 상태·중계는 http_conn이 맡는다.
 */
#define PROXY_RESPONSE_HEAD_MAX 8192 /* D-65 1단계: 종료 빈 줄 포함 응답 머리 상한 */

typedef enum {
    PROXY_BODY_NONE,   /* 204·304: 머리 끝이 응답 끝 */
    PROXY_BODY_LENGTH, /* Content-Length 바이트 수 */
    PROXY_BODY_EOF     /* 길이 없음: 백엔드 EOF까지, 클라이언트 연결은 닫는다 */
} proxy_body;

typedef enum {
    PROXY_PARSE_INCOMPLETE, /* 8KB 안에 아직 종료 빈 줄이 없다 */
    PROXY_PARSE_OK,
    PROXY_PARSE_BAD /* D-65 1~6단계 실패 → 502 */
} proxy_parse_result;

typedef struct {
    size_t head_len;       /* 종료 빈 줄 포함 머리 길이 */
    int status;            /* 200~599 */
    proxy_body body;       /* D-65 7단계 */
    size_t content_length; /* body == PROXY_BODY_LENGTH일 때 */
} proxy_response;

/* data[0..len)은 백엔드에서 받은 바이트(머리 뒤 본문이 붙어 있어도 된다). D-65 판정 순서 1~7. */
proxy_parse_result proxy_parse_response(const char *data, size_t len, proxy_response *out);

/*
 * D-64 백엔드 요청 머리: "메서드 target HTTP/1.0", 홉 머리·Host·Expect·Content-Length를 뺀 클라이언트 머리(받은 순서),
 * Host: authority, Connection: close, 원래 요청에 Content-Length가 있었으면 검증한 값. 성공이면 길이, 넘치면 -1(EINVAL).
 */
int proxy_format_request(char *buf, size_t cap, const http_request *req, const char *target, const char *authority);

/*
 * D-65 다시 쓰기: 상태 줄 버전만 HTTP/1.1로, 홉 머리와 204의 Content-Length를 빼고 connection 머리를 넣는다.
 * head는 proxy_parse_response가 OK로 판정한 머리다. 성공이면 길이, 넘치면 -1(EINVAL).
 */
int proxy_format_response_head(char *buf, size_t cap, const char *head, const proxy_response *resp,
                               http_connection connection);

#endif /* PROXY_H */
