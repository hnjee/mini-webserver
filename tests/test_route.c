/*
 * Step 6 단위 테스트: 라우팅 설정 글자 → 표(TC-6-06), 매칭·전달 주소·라운드 로빈(TC-6-05).
 */
#define _POSIX_C_SOURCE 200809L

#include "../src/route.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static route_table table; /* 크기가 커서 정적으로 둔다 */

static void check(const char *name, int condition, const char *detail)
{
    if (condition) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (%s)\n", name, detail);
        failures++;
    }
}

static int parse(const char *text, char *error, size_t size)
{
    return route_parse(text, strlen(text), &table, error, size);
}

/* 실패하고 error가 want_prefix("N행")로 시작하는가. */
static void expect_error(const char *name, const char *text, const char *want_prefix)
{
    char error[256] = "";
    int rc = route_parse(text, strlen(text), &table, error, sizeof(error));
    char label[256];
    snprintf(label, sizeof(label), "TC-6-06 %s → 실패, %s", name, want_prefix);
    check(label, rc == -1 && strncmp(error, want_prefix, strlen(want_prefix)) == 0, error[0] ? error : "성공했다");
}

static void test_parse_ok(void)
{
    char error[256] = "";
    int rc = parse("# 주석 줄\n\n  route /api/ a.example:8001 b.example:8002   # 끝 주석\r\n"
                   "route\t/api/v2/\tc:1\n",
                   error, sizeof(error));

    check("TC-6-06 유효 설정 2규칙", rc == 0 && table.count == 2, error);
    check("TC-6-06 접두·백엔드 수", strcmp(table.rules[0].prefix, "/api/") == 0 && table.rules[0].backend_count == 2 &&
                                        strcmp(table.rules[1].prefix, "/api/v2/") == 0 &&
                                        table.rules[1].backend_count == 1,
          "표가 다르다");
    check("TC-6-06 호스트·포트·Host 값", strcmp(table.rules[0].backends[1].host, "b.example") == 0 &&
                                             table.rules[0].backends[1].port == 8002 &&
                                             strcmp(table.rules[0].backends[1].authority, "b.example:8002") == 0 &&
                                             table.rules[1].backends[0].port == 1,
          "백엔드가 다르다");
    check("TC-6-06 줄 번호 기록", table.rules[0].line == 3 && table.rules[1].line == 4, "줄 번호가 다르다");
}

static void test_parse_errors(void)
{
    char host[300], text[400];

    expect_error("규칙 없음(빈 파일)", "", "설정");
    expect_error("규칙 없음(주석만)", "# x\n\n", "설정");
    expect_error("모르는 지시어", "\nroutes /a/ h:1\n", "2행");
    expect_error("백엔드 없음", "route /api/\n", "1행");
    expect_error("접두가 / 로 시작 안 함", "route api/ h:1\n", "1행");
    expect_error("접두가 / 로 끝나지 않음(/api)", "route /api h:1\n", "1행");
    expect_error("접두에 ?", "route /a?b/ h:1\n", "1행");
    expect_error("접두에 제어 문자", "route /a\x01/ h:1\n", "1행");
    expect_error("접두 중복", "route /a/ h:1\nroute /a/ h:2\n", "2행");
    expect_error("포트 0", "route /a/ h:0\n", "1행");
    expect_error("포트 65536", "route /a/ h:65536\n", "1행");
    expect_error("포트 숫자 아님", "route /a/ h:8x\n", "1행");
    expect_error("포트 없음", "route /a/ h\n", "1행");
    expect_error("빈 포트", "route /a/ h:\n", "1행");
    expect_error("빈 호스트", "route /a/ :80\n", "1행");
    {
        char error[256] = "";
        static const char nul[] = "route /a/ h\0x:1\n";
        int rc = route_parse(nul, sizeof(nul) - 1, &table, error, sizeof(error));
        check("TC-6-06 호스트 안 NUL → 실패, 1행", rc == -1 && strncmp(error, "1행", 4) == 0, error);
    }
    memset(host, 'h', 256);
    host[256] = '\0';
    snprintf(text, sizeof(text), "route /a/ %s:1\n", host);
    expect_error("호스트 256바이트", text, "1행");
    host[255] = '\0';
    snprintf(text, sizeof(text), "route /a/ %s:1\n", host);
    {
        char error[256] = "";
        check("TC-6-06 호스트 255바이트 허용", parse(text, error, sizeof(error)) == 0, error);
    }
    expect_error("백엔드 9개", "route /a/ h:1 h:2 h:3 h:4 h:5 h:6 h:7 h:8 h:9\n", "1행");
    {
        char many[2048] = "";
        int i;
        for (i = 0; i <= ROUTE_MAX_RULES; i++) {
            char line[64];
            snprintf(line, sizeof(line), "route /r%d/ h:1\n", i);
            strcat(many, line);
        }
        expect_error("규칙 17개", many, "17행");
    }
}

/* F1: "호스트:포트" 토큰이 authority 용량(261바이트 + NUL)을 넘으면 복사 전에 줄 번호 오류로 실패한다. */
static char *zeros_port_line(const char *prefix_lines, const char *head, size_t zeros)
{
    size_t len = strlen(prefix_lines) + strlen(head) + zeros + 8;
    char *t = malloc(len);
    size_t n = (size_t)snprintf(t, len, "%s%s", prefix_lines, head);
    memset(t + n, '0', zeros);
    strcpy(t + n + zeros, "1\n");
    return t;
}

static void test_long_backend_token(void)
{
    char rules[2048] = "", error[256] = "", host[300], *t;
    int i;

    t = zeros_port_line("", "route /a/ h:", 300);
    expect_error("포트 앞자리 0 300개", t, "1행");
    free(t);
    t = zeros_port_line("", "route /a/ h:", 400);
    expect_error("포트 앞자리 0 400개", t, "1행");
    free(t);
    for (i = 0; i < ROUTE_MAX_RULES - 1; i++) {
        char line[64];
        snprintf(line, sizeof(line), "route /r%d/ h:1\n", i);
        strcat(rules, line);
    }
    t = zeros_port_line(rules, "route /last/ h:1 h:2 h:3 h:4 h:5 h:6 h:7 h:", 3000);
    expect_error("16번째 규칙의 8번째 백엔드 포트 앞자리 0 3000개", t, "16행");
    free(t);

    /* 경계: 토큰 261바이트(호스트 255 + ':' + 5자리)는 허용, 한 바이트 더 길면 실패 */
    memset(host, 'h', 255);
    host[255] = '\0';
    {
        char line[400];
        snprintf(line, sizeof(line), "route /a/ %s:65535\n", host);
        check("TC-6-06 백엔드 토큰 261바이트(최대) 허용, authority 그대로",
              route_parse(line, strlen(line), &table, error, sizeof(error)) == 0 &&
                  strlen(table.rules[0].backends[0].authority) == 261 && table.rules[0].backends[0].port == 65535,
              error);
        snprintf(line, sizeof(line), "route /a/ %s:065535\n", host);
        expect_error("백엔드 토큰 262바이트(한 바이트 초과)", line, "1행");
    }
}

static void test_match(void)
{
    char error[256] = "";
    route_rule *r;

    parse("route /api/ h1:8001 h2:8002\nroute /api/v2/ h3:1\nroute /static/x/ h4:1\n", error, sizeof(error));
    check("TC-6-05 /api 는 규칙 밖", route_match(&table, "/api") == NULL, "맞았다");
    check("TC-6-05 /api?x 는 규칙 밖", route_match(&table, "/api?x") == NULL, "맞았다");
    check("TC-6-05 /API/ 대소문자 구분", route_match(&table, "/API/") == NULL, "맞았다");
    check("TC-6-05 /index.html 규칙 밖", route_match(&table, "/index.html") == NULL, "맞았다");
    check("TC-6-05 /%61pi/ 디코딩 없음", route_match(&table, "/%61pi/") == NULL, "맞았다");
    r = route_match(&table, "/api/");
    check("TC-6-05 /api/ → /api/ 규칙, 전달 /", r == &table.rules[0] && strcmp(route_forward_target(r, "/api/"), "/") == 0,
          "규칙·전달 주소가 다르다");
    r = route_match(&table, "/api/users?x=1");
    check("TC-6-05 /api/users?x=1 → 전달 /users?x=1",
          r == &table.rules[0] && strcmp(route_forward_target(r, "/api/users?x=1"), "/users?x=1") == 0, "다르다");
    r = route_match(&table, "/api/v2/item");
    check("TC-6-05 가장 긴 접두 /api/v2/ (선언 순서 무관)",
          r == &table.rules[1] && strcmp(route_forward_target(r, "/api/v2/item"), "/item") == 0, "다르다");
    r = route_match(&table, "/api/v2");
    check("TC-6-05 /api/v2 는 /api/ 규칙", r == &table.rules[0] && strcmp(route_forward_target(r, "/api/v2"), "/v2") == 0,
          "다르다");
    r = route_match(&table, "/api/../a");
    check("TC-6-05 /api/../a → /../a 그대로", r == &table.rules[0] && strcmp(route_forward_target(r, "/api/../a"), "/../a") == 0,
          "다르다");
    parse("route /api/v2/ h3:1\nroute /api/ h1:1\n", error, sizeof(error));
    check("TC-6-05 선언 순서를 바꿔도 가장 긴 규칙", route_match(&table, "/api/v2/x") == &table.rules[0], "다르다");
}

static void test_round_robin(void)
{
    char error[256] = "", seq[16] = "";
    route_rule *a, *b;
    int i;

    parse("route /a/ h:1 h:2\nroute /b/ h:3 h:4 h:5\n", error, sizeof(error));
    a = &table.rules[0];
    b = &table.rules[1];
    for (i = 0; i < 4; i++) {
        seq[i * 2] = (char)('0' + route_pick(a)->port);
        seq[i * 2 + 1] = (char)('0' + route_pick(b)->port);
    }
    check("TC-6-05 규칙마다 카운터 하나, 번갈아 고름", strcmp(seq, "13241523") == 0, seq);
}

static void test_resolve_and_load(void)
{
    char error[256] = "", path[] = "/tmp/test_route-XXXXXX";
    char addr[INET_ADDRSTRLEN];
    int fd;
    static const char conf[] = "route /api/ 127.0.0.1:8001 localhost:8002\n";

    fd = mkstemp(path);
    check("TC-6-06 준비: 임시 파일", fd >= 0 && write(fd, conf, sizeof(conf) - 1) == (ssize_t)sizeof(conf) - 1, "mkstemp");
    close(fd);
    check("TC-6-06 파일 읽기·이름 조회 성공", route_load(path, &table, error, sizeof(error)) == 0, error);
    inet_ntop(AF_INET, &table.rules[0].backends[1].addr.sin_addr, addr, sizeof(addr));
    check("TC-6-06 localhost → 127.0.0.1:8002 (IPv4)",
          table.rules[0].backends[1].addr.sin_family == AF_INET && strcmp(addr, "127.0.0.1") == 0 &&
              ntohs(table.rules[0].backends[1].addr.sin_port) == 8002,
          addr);
    unlink(path);
    check("TC-6-06 없는 파일 → 실패", route_load(path, &table, error, sizeof(error)) == -1 && error[0] != '\0', "성공했다");
    parse("\nroute /a/ 127.0.0.1:1\nroute /b/ no-such-host.invalid:80\n", error, sizeof(error));
    check("TC-6-06 이름 조회 실패 → 실패, 3행",
          route_resolve(&table, error, sizeof(error)) == -1 && strncmp(error, "3행", 4) == 0, error);
}

int main(void)
{
    printf("test_route (TC-6-05, TC-6-06)\n");
    test_parse_ok();
    test_parse_errors();
    test_long_backend_token();
    test_match();
    test_round_robin();
    test_resolve_and_load();
    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
