#ifndef ROUTE_H
#define ROUTE_H

#include <netinet/in.h>
#include <stdatomic.h>
#include <stddef.h>

/*
 * D-61~D-63: 라우팅 설정(-c)과 규칙 매칭·라운드 로빈.
 * 표는 main이 시작할 때 만들고 프로세스가 끝날 때까지 빌려준다. 실행 중 이름 조회는 하지 않는다.
 * 규칙 수·백엔드 수 상한은 D-61이 구현에 맡긴 값이다.
 */
#define ROUTE_MAX_RULES 16
#define ROUTE_MAX_BACKENDS 8
#define ROUTE_HOST_MAX 255     /* D-61: 호스트 255바이트 이하 */
#define ROUTE_PREFIX_MAX 2048  /* 요청 대상 상한(D-43)보다 긴 접두는 맞을 수 없다 */
#define ROUTE_FILE_MAX 65536   /* 설정 파일 크기 상한 */

typedef struct {
    char host[ROUTE_HOST_MAX + 1];
    char authority[ROUTE_HOST_MAX + 7]; /* 백엔드 요청의 Host 값: "호스트:포트" */
    int port;
    struct sockaddr_in addr; /* route_resolve가 채운다 */
} route_backend;

typedef struct {
    char prefix[ROUTE_PREFIX_MAX + 1];
    size_t prefix_len;
    route_backend backends[ROUTE_MAX_BACKENDS];
    size_t backend_count;
    atomic_ulong next;  /* D-63: 다음에 고를 차례. 워커 여럿이 동시에 고르므로 원자 증가한다(워커 확장) */
    int line;           /* 설정 파일 줄 번호(이름 조회 실패 보고용) */
} route_rule;

typedef struct {
    route_rule rules[ROUTE_MAX_RULES];
    size_t count;
} route_table;

/* 설정 글자 text[0..len)을 표로 바꾼다(이름 조회 없음). 실패면 -1이고 error에 "N행: 이유"를 쓴다. */
int route_parse(const char *text, size_t len, route_table *out, char *error, size_t error_size);

/* 모든 호스트를 IPv4 주소로 한 번 바꾼다(블로킹). 실패면 -1, error에 줄 번호와 이유. */
int route_resolve(route_table *table, char *error, size_t error_size);

/* 파일을 읽어 parse + resolve. 실패면 -1, error에 이유. */
int route_load(const char *path, route_table *out, char *error, size_t error_size);

/* D-62: target이 바이트 그대로 시작하는 가장 긴 접두의 규칙. 없으면 NULL. */
route_rule *route_match(route_table *table, const char *target);

/* D-60·D-62: 접두를 "/" 하나로 바꾼 전달 주소. target 안을 가리킨다(복사 없음). */
const char *route_forward_target(const route_rule *rule, const char *target);

/* D-63: 규칙의 다음 백엔드를 고르고 카운터를 바로 1 늘린다. 여러 스레드에서 동시에 불러도 된다. */
const route_backend *route_pick(route_rule *rule);

#endif /* ROUTE_H */
