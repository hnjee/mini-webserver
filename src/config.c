#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT_MIN 1
#define PORT_MAX 65535

/*
 * 문자열을 포트 번호로 바꾼다. 성공하면 0, 실패하면 -1.
 * atoi()는 "abc"에도 0을 돌려줘서 실패를 알 수 없으므로 strtol()을 쓴다.
 */
static int parse_port(const char *s, int *out)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0)        /* 범위를 넘는 큰 수 */
        return -1;
    if (end == s)          /* 숫자가 하나도 없음: "abc", "" */
        return -1;
    if (*end != '\0')      /* 숫자 뒤에 다른 글자: "80abc" */
        return -1;
    if (value < PORT_MIN || value > PORT_MAX)
        return -1;

    *out = (int)value;
    return 0;
}

static int is_directory(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_regular_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

enum config_result config_parse(int argc, char *argv[], struct server_config *cfg)
{
    const char *port_str = NULL;
    int opt;

    cfg->port = 0;
    cfg->root = NULL;
    cfg->routes_path = NULL;

    /* 맨 앞 ':'와 opterr = 0: getopt() 대신 직접 에러 메시지를 출력한다 */
    opterr = 0;
    optind = 1;
    while ((opt = getopt(argc, argv, ":p:r:c:h")) != -1) {
        switch (opt) {
        case 'p':
            port_str = optarg;
            break;
        case 'r':
            cfg->root = optarg;
            break;
        case 'c':
            cfg->routes_path = optarg;
            break;
        case 'h':
            return CONFIG_HELP;
        case ':':
            fprintf(stderr, "error: -%c 옵션에는 값이 필요해요\n", optopt);
            return CONFIG_ERROR;
        default:
            fprintf(stderr, "error: 알 수 없는 옵션이에요: -%c\n", optopt);
            return CONFIG_ERROR;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "error: 옵션이 아닌 인자가 있어요: %s\n", argv[optind]);
        return CONFIG_ERROR;
    }
    if (port_str == NULL) {
        fprintf(stderr, "error: -p (포트)가 필요해요\n");
        return CONFIG_ERROR;
    }
    if (cfg->root == NULL) {
        fprintf(stderr, "error: -r (루트 디렉터리)가 필요해요\n");
        return CONFIG_ERROR;
    }
    if (parse_port(port_str, &cfg->port) != 0) {
        fprintf(stderr, "error: 포트는 %d~%d 정수여야 해요: %s\n",
                PORT_MIN, PORT_MAX, port_str);
        return CONFIG_ERROR;
    }
    if (!is_directory(cfg->root)) {
        fprintf(stderr, "error: 루트 디렉터리가 없어요: %s\n", cfg->root);
        return CONFIG_ERROR;
    }
    if (cfg->routes_path != NULL && !is_regular_file(cfg->routes_path)) {
        fprintf(stderr, "error: 라우팅 설정 파일이 없어요: %s\n", cfg->routes_path);
        return CONFIG_ERROR;
    }

    return CONFIG_OK;
}

void config_print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "사용법: %s -p <포트> -r <루트 디렉터리> [-c <라우팅 설정 파일>]\n"
            "  -p  서버가 열 포트 번호 (1~65535)      예) -p 8080\n"
            "  -r  정적 파일 루트 디렉터리            예) -r ./www\n"
            "  -c  리버스 프록시 라우팅 설정 파일     예) -c ./routes.conf\n"
            "  -h  이 사용법을 출력\n",
            prog);
}
