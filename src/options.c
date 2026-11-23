#define _POSIX_C_SOURCE 200809L

#include "options.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT_MIN 1
#define PORT_MAX 65535

static int parse_port(const char *text, int *out, char *error, size_t error_size)
{
    char *end = NULL;
    long value;

    if (text == NULL || *text == '\0') {
        snprintf(error, error_size, "-p: 포트 번호가 비어 있다");
        return -1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        snprintf(error, error_size, "-p: 포트 번호가 정수가 아니다: %s", text);
        return -1;
    }
    if (value < PORT_MIN || value > PORT_MAX) {
        snprintf(error, error_size, "-p: 포트 번호는 %d~%d 범위여야 한다: %s",
                 PORT_MIN, PORT_MAX, text);
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int check_root(const char *path, char *error, size_t error_size)
{
    struct stat info;

    if (path == NULL || *path == '\0') {
        snprintf(error, error_size, "-r: 루트 경로가 비어 있다");
        return -1;
    }
    if (stat(path, &info) != 0) {
        snprintf(error, error_size, "-r: 경로를 열 수 없다: %s (%s)", path, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(info.st_mode)) {
        snprintf(error, error_size, "-r: 디렉터리가 아니다: %s", path);
        return -1;
    }
    return 0;
}

void options_usage(void *stream, const char *program_name)
{
    FILE *out = (FILE *)stream;

    fprintf(out, "사용법: %s -p <포트> -r <루트 디렉터리>\n", program_name);
    fprintf(out, "  -p  서버가 열 포트 번호 (%d~%d)\n", PORT_MIN, PORT_MAX);
    fprintf(out, "  -r  정적 파일 루트 디렉터리 (존재해야 한다, Step 3부터 사용)\n");
    fprintf(out, "  -h  이 도움말을 출력한다\n");
}

options_status options_parse(int argc, char *const argv[], options *out,
                             char *error, size_t error_size)
{
    int option;
    const char *port_text = NULL;

    out->port = 0;
    out->root = NULL;
    if (error_size > 0) {
        error[0] = '\0';
    }

    /* glibc는 optind = 0일 때만 묶음 옵션(-zx) 중간 위치까지 내부 상태를 초기화한다.
       optind = 1로는 이전 호출의 잔여 문자가 남는다. 이 저장소는 Linux(glibc)만 지원한다. */
    optind = 0;
    opterr = 0;  /* getopt가 직접 메시지를 찍지 않게 한다 */
    while ((option = getopt(argc, argv, ":p:r:h")) != -1) {
        switch (option) {
        case 'p':
            port_text = optarg;
            break;
        case 'r':
            out->root = optarg;
            break;
        case 'h':
            return OPTIONS_HELP;
        case ':':
            snprintf(error, error_size, "-%c: 값이 빠졌다", optopt);
            return OPTIONS_ERROR;
        default:
            snprintf(error, error_size, "알 수 없는 인자: -%c", optopt);
            return OPTIONS_ERROR;
        }
    }

    if (optind < argc) {
        snprintf(error, error_size, "해석할 수 없는 인자: %s", argv[optind]);
        return OPTIONS_ERROR;
    }
    if (port_text == NULL) {
        snprintf(error, error_size, "-p: 포트 번호가 필요하다");
        return OPTIONS_ERROR;
    }
    if (out->root == NULL) {
        snprintf(error, error_size, "-r: 루트 디렉터리가 필요하다");
        return OPTIONS_ERROR;
    }
    if (parse_port(port_text, &out->port, error, error_size) != 0) {
        return OPTIONS_ERROR;
    }
    if (check_root(out->root, error, error_size) != 0) {
        return OPTIONS_ERROR;
    }
    return OPTIONS_OK;
}
