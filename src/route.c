/*
 * 라우팅 표: docs/design/step6.md 2장 (S6-02, S6-05)
 * routes.conf를 켤 때 한 번 읽고, 그 뒤로는 경로 맞추기와 라운드 로빈만 해요.
 */
#include "route.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "127.0.0.1:8001"을 주소로 바꿔요. 성공 0, 실패 -1 */
static int parse_backend(const char *s, struct backend *b)
{
    const char *colon = strrchr(s, ':');
    if (colon == NULL || colon == s || colon - s >= 16)
        return -1;
    char ip[16];
    memcpy(ip, s, (size_t)(colon - s));
    ip[colon - s] = '\0';

    char *end = NULL;
    errno = 0;
    long port = strtol(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' || port < 1 || port > 65535)
        return -1;

    memset(b, 0, sizeof(*b));
    b->addr.sin_family = AF_INET;
    b->addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &b->addr.sin_addr) != 1)
        return -1;                               /* IPv4 숫자만 받아요 (이름은 getaddrinfo가 기다릴 수 있어서) */
    snprintf(b->name, sizeof(b->name), "%s:%ld", ip, port);
    return 0;
}

int routes_load(const char *path, struct routes *out)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "error: %s를 열 수 없습니다\n", path);
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        char *save = NULL;
        char *tok = strtok_r(line, " \t\r\n", &save);
        if (tok == NULL || tok[0] == '#')
            continue;                            /* 빈 줄, 주석 */

        const char *why = NULL;
        struct route *rt = &out->items[out->n];
        if (out->n == ROUTE_MAX)
            why = "규칙이 너무 많습니다";
        else if (tok[0] != '/' || strlen(tok) >= ROUTE_PREFIX_MAX)
            why = "경로는 /로 시작해야 합니다";
        else {
            memset(rt, 0, sizeof(*rt));
            strcpy(rt->prefix, tok);
            while (why == NULL && (tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
                if (rt->nbackends == ROUTE_MAX_BACKENDS)
                    why = "백엔드가 너무 많습니다";
                else if (parse_backend(tok, &rt->backends[rt->nbackends]) < 0)
                    why = "백엔드는 IPv4:포트 형식이어야 합니다";
                else
                    rt->nbackends++;
            }
            if (why == NULL && rt->nbackends == 0)
                why = "백엔드가 없습니다";
        }
        if (why != NULL) {
            fprintf(stderr, "error: %s %d번째 줄: %s\n", path, lineno, why);
            fclose(f);
            return -1;
        }
        atomic_init(&rt->next, 0);
        out->n++;
    }
    fclose(f);
    return 0;
}

struct route *routes_match(struct routes *r, const char *path)
{
    struct route *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < r->n; i++) {
        size_t n = strlen(r->items[i].prefix);
        if (n > best_len && strncmp(path, r->items[i].prefix, n) == 0) {
            best = &r->items[i];
            best_len = n;
        }
    }
    return best;
}

const struct backend *route_pick(struct route *rt)
{
    unsigned k = atomic_fetch_add(&rt->next, 1);
    return &rt->backends[k % (unsigned)rt->nbackends];
}
