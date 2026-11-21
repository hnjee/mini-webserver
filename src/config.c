#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* 포트 문자열을 1~65535 정수로 바꾼다. 실패하면 -1 */
static int parse_port(const char *s)
{
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;  /* 범위 초과, 숫자 없음("abc"), 뒤에 글자 남음("80a") */
    }
    if (v < 1 || v > 65535) {
        return -1;
    }
    return (int)v;
}

/* 경로가 존재하는 디렉터리면 1 */
static int is_directory(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

enum config_result config_parse(int argc, char *argv[], struct server_config *out)
{
    const char *port_str = NULL;
    const char *root = NULL;
    const char *config_path = NULL;
    int opt;

    optind = 1;  /* 테스트에서 여러 번 불러도 처음부터 읽게 한다 */
    opterr = 0;  /* getopt가 직접 에러를 출력하지 않게 한다. 출력은 main()의 몫 */

    while ((opt = getopt(argc, argv, "p:r:c:")) != -1) {
        /* 같은 인자가 두 번 오면 실수로 본다 (S0-A08). 마지막 값으로 덮어쓰지 않는다 */
        switch (opt) {
        case 'p':
            if (port_str != NULL) {
                return CONFIG_ERR_USAGE;
            }
            port_str = optarg;
            break;
        case 'r':
            if (root != NULL) {
                return CONFIG_ERR_USAGE;
            }
            root = optarg;
            break;
        case 'c':
            if (config_path != NULL) {
                return CONFIG_ERR_USAGE;
            }
            config_path = optarg;
            break;
        default:  /* 모르는 인자, 값이 빠진 인자 */
            return CONFIG_ERR_USAGE;
        }
    }

    if (optind < argc || port_str == NULL || root == NULL) {
        return CONFIG_ERR_USAGE;  /* 남는 인자, 필수 인자 빠짐 */
    }

    out->port = parse_port(port_str);
    if (out->port < 0) {
        return CONFIG_ERR_PORT;
    }

    if (!is_directory(root)) {
        return CONFIG_ERR_ROOT;
    }

    out->root = root;
    out->config_path = config_path;
    return CONFIG_OK;
}

void config_print_usage(const char *prog)
{
    fprintf(stderr, "usage: %s -p PORT -r ROOT [-c CONFIG]\n", prog);
}
