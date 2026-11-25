/*
 * Step 0 단위 테스트: docs/tests/step0.md 4장 UT-01 ~ UT-17
 * Step 6 단위 테스트: docs/tests/step6.md 3장 -t (이름 앞에 S6을 붙인다)
 * 저장소 루트에서 실행한다 (make test). ./www 경로를 쓰기 때문이다.
 */
#include "config.h"

#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_fail = 0;

#define ARGC(a) ((int)(sizeof(a) / sizeof((a)[0])) - 1)

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

static int str_eq(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* UT-01: 올바른 -p, -r */
static void ut01_ok_port_root(void)
{
    char *argv[] = {"webserver", "-p", "8080", "-r", "./www", NULL};
    struct server_config c = {0};
    enum config_result r = config_parse(ARGC(argv), argv, &c);
    CHECK("UT-01 결과 OK", r == CONFIG_OK);
    CHECK("UT-01 port == 8080", c.port == 8080);
    CHECK("UT-01 root == ./www", str_eq(c.root, "./www"));
}

/* UT-02: -c 저장 */
static void ut02_config_path(void)
{
    char *argv[] = {"webserver", "-p", "8080", "-r", "./www", "-c", "routes.conf", NULL};
    struct server_config c = {0};
    enum config_result r = config_parse(ARGC(argv), argv, &c);
    CHECK("UT-02 결과 OK", r == CONFIG_OK);
    CHECK("UT-02 config_path == routes.conf", str_eq(c.config_path, "routes.conf"));
}

/* UT-03: -c 없으면 NULL */
static void ut03_no_config_path(void)
{
    char *argv[] = {"webserver", "-p", "8080", "-r", "./www", NULL};
    struct server_config c = {.config_path = "쓰레기 값"};
    enum config_result r = config_parse(ARGC(argv), argv, &c);
    CHECK("UT-03 결과 OK", r == CONFIG_OK);
    CHECK("UT-03 config_path == NULL", c.config_path == NULL);
}

/* 결과 코드 하나만 확인하는 경우를 묶는다 */
static void expect(const char *id, char *argv[], int argc, enum config_result want)
{
    struct server_config c = {0};
    CHECK(id, config_parse(argc, argv, &c) == want);
}

static void ut04_to_ut13(void)
{
    char *a04[] = {"webserver", "-p", "8080", NULL};
    char *a05[] = {"webserver", "-r", "./www", NULL};
    char *a06[] = {"webserver", "-p", "abc", "-r", "./www", NULL};
    char *a07[] = {"webserver", "-p", "80a", "-r", "./www", NULL};
    char *a08[] = {"webserver", "-p", "0", "-r", "./www", NULL};
    char *a09[] = {"webserver", "-p", "65536", "-r", "./www", NULL};
    char *a10[] = {"webserver", "-p", "65535", "-r", "./www", NULL};
    char *a11[] = {"webserver", "-p", "8080", "-r", "./없음", NULL};
    char *a12[] = {"webserver", "-p", "8080", "-r", "./www/index.html", NULL};
    char *a13[] = {"webserver", "-x", NULL};

    expect("UT-04 -p만 있음 → USAGE",       a04, ARGC(a04), CONFIG_ERR_USAGE);
    expect("UT-05 -r만 있음 → USAGE",       a05, ARGC(a05), CONFIG_ERR_USAGE);
    expect("UT-06 -p abc → PORT",           a06, ARGC(a06), CONFIG_ERR_PORT);
    expect("UT-07 -p 80a → PORT",           a07, ARGC(a07), CONFIG_ERR_PORT);
    expect("UT-08 -p 0 → PORT",             a08, ARGC(a08), CONFIG_ERR_PORT);
    expect("UT-09 -p 65536 → PORT",         a09, ARGC(a09), CONFIG_ERR_PORT);
    expect("UT-10 -p 65535 → OK (경곗값)",  a10, ARGC(a10), CONFIG_OK);
    expect("UT-11 없는 루트 → ROOT",        a11, ARGC(a11), CONFIG_ERR_ROOT);
    expect("UT-12 파일인 루트 → ROOT",      a12, ARGC(a12), CONFIG_ERR_ROOT);
    expect("UT-13 모르는 인자 -x → USAGE",  a13, ARGC(a13), CONFIG_ERR_USAGE);
}

/* UT-14: 두 번 이어서 불러도 getopt 상태가 남지 않아야 한다 (optind 초기화) */
static void ut14_call_twice(void)
{
    char *first[] = {"webserver", "-p", "8080", "-r", "./www", NULL};
    char *second[] = {"webserver", "-p", "8080", NULL};
    struct server_config c = {0};
    config_parse(ARGC(first), first, &c);
    CHECK("UT-14 두 번째 호출도 USAGE", config_parse(ARGC(second), second, &c) == CONFIG_ERR_USAGE);
}

/* UT-15~17: 같은 인자를 두 번 주면 실수로 보고 사용법 오류 (S0-A08) */
static void ut15_to_ut17_duplicates(void)
{
    char *a15[] = {"webserver", "-p", "8080", "-p", "9090", "-r", "./www", NULL};
    char *a16[] = {"webserver", "-p", "8080", "-r", "./www", "-r", "./www", NULL};
    char *a17[] = {"webserver", "-p", "8080", "-r", "./www", "-c", "a.conf", "-c", "b.conf", NULL};

    expect("UT-15 -p 두 번 → USAGE", a15, ARGC(a15), CONFIG_ERR_USAGE);
    expect("UT-16 -r 두 번 → USAGE", a16, ARGC(a16), CONFIG_ERR_USAGE);
    expect("UT-17 -c 두 번 → USAGE", a17, ARGC(a17), CONFIG_ERR_USAGE);
}

/* Step 6: -t 워커 수 (S6-01) */
static void s6_threads(void)
{
    char *a1[] = {"webserver", "-p", "8080", "-r", "./www", "-t", "4", NULL};
    struct server_config c = {0};
    CHECK("S6 UT-09 -t 4 → OK", config_parse(ARGC(a1), a1, &c) == CONFIG_OK);
    CHECK("S6 UT-09 threads == 4", c.threads == 4);

    char *a2[] = {"webserver", "-p", "8080", "-r", "./www", NULL};
    struct server_config d = {.threads = 99};
    CHECK("S6 UT-10 -t 없음 → threads == 0", config_parse(ARGC(a2), a2, &d) == CONFIG_OK && d.threads == 0);

    char *a3[] = {"webserver", "-p", "8080", "-r", "./www", "-t", "1", NULL};
    char *a4[] = {"webserver", "-p", "8080", "-r", "./www", "-t", "256", NULL};
    char *a5[] = {"webserver", "-p", "8080", "-r", "./www", "-t", "0", NULL};
    char *a6[] = {"webserver", "-p", "8080", "-r", "./www", "-t", "257", NULL};
    char *a7[] = {"webserver", "-p", "8080", "-r", "./www", "-t", "abc", NULL};
    char *a8[] = {"webserver", "-p", "8080", "-r", "./www", "-t", "2", "-t", "3", NULL};
    expect("S6 UT-11 -t 1 → OK (경곗값)",   a3, ARGC(a3), CONFIG_OK);
    expect("S6 UT-11 -t 256 → OK (경곗값)", a4, ARGC(a4), CONFIG_OK);
    expect("S6 UT-12 -t 0 → THREADS",       a5, ARGC(a5), CONFIG_ERR_THREADS);
    expect("S6 UT-12 -t 257 → THREADS",     a6, ARGC(a6), CONFIG_ERR_THREADS);
    expect("S6 UT-12 -t abc → THREADS",     a7, ARGC(a7), CONFIG_ERR_THREADS);
    expect("S6 UT-13 -t 두 번 → USAGE",     a8, ARGC(a8), CONFIG_ERR_USAGE);
}

int main(void)
{
    printf("test_config\n");
    ut01_ok_port_root();
    ut02_config_path();
    ut03_no_config_path();
    ut04_to_ut13();
    ut14_call_twice();
    ut15_to_ut17_duplicates();
    s6_threads();
    printf("%d개 중 %d개 통과\n", g_run, g_run - g_fail);
    return g_fail == 0 ? 0 : 1;
}
