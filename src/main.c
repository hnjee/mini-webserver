/*
 * Step 2: HTTP 요청 파싱 + 고정 응답.
 * 인자를 확인하고 듣는 소켓을 연 뒤, 연결을 하나씩 받아 요청 하나에 고정 응답 하나를 보내고 닫는다.
 * 서버는 Ctrl+C(기본 동작)로만 끝난다. 정상 종료 처리는 Step 7이다.
 */
#define _DEFAULT_SOURCE /* accept(2)의 ENONET 등 Linux 오류 코드 */

#include "http_conn.h"
#include "listener.h"
#include "options.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* D-07: 일시적인 accept 실패(Linux accept(2)가 재시도를 권하는 오류)인가. */
static int accept_retryable(int err)
{
    switch (err) {
    case EINTR:
    case ECONNABORTED:
    case EPROTO:
    case ENETDOWN:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
        return 1;
    default:
        return 0;
    }
}

int main(int argc, char *argv[])
{
    options config;
    char error[256];
    options_status status;
    int listen_fd;

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

    /* D-05: 끊긴 연결에 쓰면 프로세스가 죽지 않고 write가 EPIPE로 실패하게 한다. */
    signal(SIGPIPE, SIG_IGN);

    listen_fd = listener_open(config.port, error, sizeof(error));
    if (listen_fd < 0) {
        fprintf(stderr, "오류: %s\n", error);
        return EXIT_FAILURE;
    }
    printf("HTTP 서버 시작: 0.0.0.0:%d (루트 %s는 Step 3부터 사용). Ctrl+C로 종료\n",
           config.port, config.root);
    fflush(stdout);

    /* 한 번에 한 연결씩 처리한다. 동시성은 Step 5다. */
    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (accept_retryable(errno)) {
                continue;
            }
            fprintf(stderr, "오류: accept 실패: %s\n", strerror(errno));
            close(listen_fd);
            return EXIT_FAILURE;
        }
        if (http_conn_serve(conn_fd) == HTTP_CONN_ERROR) {
            fprintf(stderr, "연결 오류로 닫음: %s\n", strerror(errno));
        }
        close(conn_fd);
    }
}
