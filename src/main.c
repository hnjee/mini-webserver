/*
 * Step 5: 동시성(I/O 멀티플렉싱, D-55).
 * 인자를 확인하고 루트를 정규화하고 듣는 소켓을 연 뒤, 연결 처리는 epoll 루프에 맡긴다(D-56, D-57).
 * 서버는 Ctrl+C(기본 동작)로만 끝난다. 정상 종료 처리는 Step 7이다.
 */
#define _DEFAULT_SOURCE /* realpath */

#include "listener.h"
#include "options.h"
#include "server_loop.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    options config;
    char error[256];
    options_status status;
    int listen_fd;
    char *root;

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

    /* D-26: 루트를 한 번 정규화한다. 이 문자열은 프로세스 수명 동안 쓰므로 해제하지 않는다. */
    root = realpath(config.root, NULL);
    if (root == NULL) {
        fprintf(stderr, "오류: 루트 %s 를 정규화할 수 없다: %s\n", config.root, strerror(errno));
        return EXIT_FAILURE;
    }

    /* D-05: 끊긴 연결에 쓰면 프로세스가 죽지 않고 write가 EPIPE로 실패하게 한다. */
    signal(SIGPIPE, SIG_IGN);

    listen_fd = listener_open(config.port, error, sizeof(error));
    if (listen_fd < 0) {
        fprintf(stderr, "오류: %s\n", error);
        return EXIT_FAILURE;
    }
    printf("HTTP 서버 시작: 0.0.0.0:%d, 루트 %s. Ctrl+C로 종료\n", config.port, root);
    fflush(stdout);

    server_loop_run(listen_fd, root, error, sizeof(error)); /* 서버를 끝내는 실패에서만 돌아온다 */
    fprintf(stderr, "오류: %s\n", error);
    close(listen_fd);
    return EXIT_FAILURE;
}
