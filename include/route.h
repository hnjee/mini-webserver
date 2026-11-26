#ifndef ROUTE_H
#define ROUTE_H

/* 라우팅 표: docs/design/step6.md 2장 (S6-02, S6-05) */

#include <netinet/in.h>
#include <stdatomic.h>

#define ROUTE_MAX 16              /* 규칙 최대 개수 */
#define ROUTE_MAX_BACKENDS 8      /* 규칙 하나의 백엔드 최대 개수 */
#define ROUTE_PREFIX_MAX 256      /* 경로 최대 길이 */

struct backend {
    struct sockaddr_in addr;      /* connect()에 그대로 넘길 주소 */
    char name[32];                /* "127.0.0.1:8001", 로그용 */
};

struct route {
    char prefix[ROUTE_PREFIX_MAX];
    struct backend backends[ROUTE_MAX_BACKENDS];
    int nbackends;
    atomic_uint next;             /* 라운드 로빈 순번, 모든 워커가 같이 씀 */
};

struct routes {
    struct route items[ROUTE_MAX];
    int n;
};

/* routes.conf를 읽어 out을 채워요. 한 줄에 "경로 IPv4:포트 ...", #과 빈 줄은 건너뜀.
   성공 0, 실패 -1 (파일 이름과 몇 번째 줄인지 표준 에러에 출력) */
int routes_load(const char *path, struct routes *out);

/* path가 prefix로 시작하는 규칙 중 가장 긴 것. 없으면 NULL */
struct route *routes_match(struct routes *r, const char *path);

/* 라운드 로빈으로 다음 백엔드를 골라요. 스레드에 안전해요 */
const struct backend *route_pick(struct route *rt);

#endif
