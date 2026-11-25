/*
 * 라우팅 표 단위 테스트: docs/tests/step6.md 3장 (S6-02, S6-05)
 * routes_load(), routes_match(), route_pick()을 직접 부른다.
 * routes.conf는 /tmp 아래에 테스트마다 만들어 쓴다.
 */
#include "route.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_run = 0;
static int g_fail = 0;

#define CHECK(id, cond)                                                   \
    do {                                                                  \
        g_run++;                                                          \
        if (cond) {                                                       \
            printf("  ok    %s\n", id);                                   \
        } else {                                                          \
            g_fail++;                                                     \
            printf("  FAIL  %s  (%s:%d) %s\n", id, __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static char g_path[] = "/tmp/ws_routes_XXXXXX";

/* 내용으로 routes.conf를 새로 쓰고 routes_load() 결과를 돌려준다 */
static int load(const char *text, struct routes *r)
{
    FILE *f = fopen(g_path, "w");
    if (f == NULL)
        return -2;
    fputs(text, f);
    fclose(f);
    memset(r, 0, sizeof(*r));
    return routes_load(g_path, r);
}

static int port_of(const struct backend *b)
{
    return ntohs(b->addr.sin_port);
}

static struct routes g_r;

static void ut01_to_ut05_load_and_match(void)
{
    int rc = load("/api/ 127.0.0.1:8001 127.0.0.1:8002\n", &g_r);
    CHECK("UT-01 규칙 한 줄 읽기 → 0", rc == 0);
    CHECK("UT-01 규칙 1개, 백엔드 2개", g_r.n == 1 && g_r.items[0].nbackends == 2);
    CHECK("UT-01 경로 /api/", strcmp(g_r.items[0].prefix, "/api/") == 0);
    CHECK("UT-01 포트 8001, 8002", port_of(&g_r.items[0].backends[0]) == 8001 && port_of(&g_r.items[0].backends[1]) == 8002);
    CHECK("UT-01 주소 127.0.0.1", g_r.items[0].backends[0].addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK));

    rc = load("# 주석\n\n   \n/api/\t127.0.0.1:8001\n# 끝\n", &g_r);
    CHECK("UT-02 주석, 빈 줄, 탭 → 규칙 1개", rc == 0 && g_r.n == 1 && g_r.items[0].nbackends == 1);

    rc = load("/api/ 127.0.0.1:8001\n/api/v2/ 127.0.0.1:9001\n", &g_r);
    struct route *a = routes_match(&g_r, "/api/v2/x");
    struct route *b = routes_match(&g_r, "/api/x");
    CHECK("UT-03 긴 경로 먼저: /api/v2/x → /api/v2/", rc == 0 && a != NULL && strcmp(a->prefix, "/api/v2/") == 0);
    CHECK("UT-03 /api/x → /api/", b != NULL && strcmp(b->prefix, "/api/") == 0);

    CHECK("UT-04 /index.html → NULL", routes_match(&g_r, "/index.html") == NULL);
    CHECK("UT-04 /api (슬래시 없음) → NULL", routes_match(&g_r, "/api") == NULL);

    struct route *q = routes_match(&g_r, "/api/x?y=1");
    CHECK("UT-05 쿼리가 붙어도 /api/", q != NULL && strcmp(q->prefix, "/api/") == 0);
}

static void ut06_errors(void)
{
    CHECK("UT-06 백엔드 없는 줄 → -1", load("/api/\n", &g_r) == -1);
    CHECK("UT-06 포트가 숫자 아님 → -1", load("/api/ 127.0.0.1:abc\n", &g_r) == -1);
    CHECK("UT-06 포트 0 → -1", load("/api/ 127.0.0.1:0\n", &g_r) == -1);
    CHECK("UT-06 포트 70000 → -1", load("/api/ 127.0.0.1:70000\n", &g_r) == -1);
    CHECK("UT-06 이름 주소(localhost) → -1", load("/api/ localhost:8001\n", &g_r) == -1);
    CHECK("UT-06 콜론 없음 → -1", load("/api/ 127.0.0.1\n", &g_r) == -1);
    CHECK("UT-06 경로가 /로 시작 안 함 → -1", load("api/ 127.0.0.1:8001\n", &g_r) == -1);

    char many[4096] = "";
    for (int i = 0; i < ROUTE_MAX + 1; i++) {
        char line[64];
        snprintf(line, sizeof(line), "/r%d/ 127.0.0.1:8001\n", i);
        strcat(many, line);
    }
    CHECK("UT-06 규칙 17개 → -1", load(many, &g_r) == -1);

    char wide[1024] = "/api/";
    for (int i = 0; i < ROUTE_MAX_BACKENDS + 1; i++)
        strcat(wide, " 127.0.0.1:8001");
    strcat(wide, "\n");
    CHECK("UT-06 백엔드 9개 → -1", load(wide, &g_r) == -1);

    memset(&g_r, 0, sizeof(g_r));
    CHECK("UT-06 없는 파일 → -1", routes_load("/tmp/ws_no_such_routes.conf", &g_r) == -1);
}

static void ut07_round_robin(void)
{
    load("/api/ 127.0.0.1:8001 127.0.0.1:8002\n", &g_r);
    struct route *rt = routes_match(&g_r, "/api/x");
    int p[4];
    for (int i = 0; i < 4; i++)
        p[i] = rt != NULL ? port_of(route_pick(rt)) : -1;
    CHECK("UT-07 8001, 8002, 8001, 8002", p[0] == 8001 && p[1] == 8002 && p[2] == 8001 && p[3] == 8002);
}

#define PICKS 10000
static int g_count[2];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void *picker(void *arg)
{
    struct route *rt = arg;
    int local[2] = {0, 0};
    for (int i = 0; i < PICKS; i++)
        local[port_of(route_pick(rt)) == 8001 ? 0 : 1]++;
    pthread_mutex_lock(&g_mu);
    g_count[0] += local[0];
    g_count[1] += local[1];
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static void ut08_threads(void)
{
    load("/api/ 127.0.0.1:8001 127.0.0.1:8002\n", &g_r);
    struct route *rt = routes_match(&g_r, "/api/x");
    if (rt == NULL) {
        CHECK("UT-08 규칙 찾기", 0);
        return;
    }
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, picker, rt);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    CHECK("UT-08 스레드 4개가 1만 번씩 → 정확히 2만, 2만", g_count[0] == 2 * PICKS && g_count[1] == 2 * PICKS);
}

int main(void)
{
    printf("test_route\n");
    int fd = mkstemp(g_path);
    if (fd < 0) {
        printf("  FAIL  임시 파일을 만들지 못함\n");
        return 1;
    }
    close(fd);
    ut01_to_ut05_load_and_match();
    ut06_errors();
    ut07_round_robin();
    ut08_threads();
    unlink(g_path);
    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
