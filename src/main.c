/*
 * Step 0: 빌드와 실행 인자만 준비한다.
 * 아직 소켓을 열지 않으므로, 인자를 확인하고 바로 끝낸다.
 * Step 1에서 이 자리에 socket/bind/listen/accept 루프가 들어간다.
 */
#define _POSIX_C_SOURCE 200809L

#include "options.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    options config;
    char error[256];
    options_status status;

    status = options_parse(argc, argv, &config, error, sizeof(error));
    if (status == OPTIONS_HELP) {
        options_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (status != OPTIONS_OK) {
        fprintf(stderr, "오류: %s\n", error);
        options_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    printf("설정 확인\n");
    printf("  포트: %d\n", config.port);
    printf("  루트: %s\n", config.root);
    printf("Step 0까지는 인자만 확인하고 끝낸다. 아직 포트를 열거나 연결을 받지 않는다.\n");
    return EXIT_SUCCESS;
}
