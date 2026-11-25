/* realpath()는 POSIX의 XSI 부분이라 _XOPEN_SOURCE를 켜야 선언된다 */
#define _XOPEN_SOURCE 700

#include "config.h"
#include "route.h"
#include "server.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    struct server_config cfg = {0};

    switch (config_parse(argc, argv, &cfg)) {
    case CONFIG_OK:
        break;
    case CONFIG_ERR_USAGE:
        config_print_usage(argv[0]);
        return EXIT_FAILURE;
    case CONFIG_ERR_PORT:
        fprintf(stderr, "error: 포트는 1~65535 사이의 정수여야 합니다\n");
        return EXIT_FAILURE;
    case CONFIG_ERR_ROOT:
        fprintf(stderr, "error: 정적 파일 루트가 없거나 디렉터리가 아닙니다\n");
        return EXIT_FAILURE;
    case CONFIG_ERR_THREADS:
        fprintf(stderr, "error: 워커 수(-t)는 1~256 사이의 정수여야 합니다\n");
        return EXIT_FAILURE;
    }

    printf("port: %d\n", cfg.port);
    printf("root: %s\n", cfg.root);
    if (cfg.config_path != NULL) {
        printf("config: %s\n", cfg.config_path);
    }

    /* 라우팅 표: -c가 있으면 켤 때 한 번 읽어요 (S6-02). 형식이 틀리면 켜지 않아요 */
    static struct routes routes;
    struct routes *rp = NULL;
    if (cfg.config_path != NULL) {
        if (routes_load(cfg.config_path, &routes) < 0)
            return EXIT_FAILURE;
        rp = &routes;
        for (int i = 0; i < routes.n; i++) {
            printf("route: %s ->", routes.items[i].prefix);
            for (int k = 0; k < routes.items[i].nbackends; k++)
                printf(" %s", routes.items[i].backends[k].name);
            printf("\n");
        }
    }

    /* 워커 수: -t가 없으면 코어 수 × 2 (S6-01, 결정 18행) */
    int workers = cfg.threads;
    if (workers == 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        workers = cores > 0 ? (int)cores * 2 : 2;
        if (workers > WORKERS_MAX)
            workers = WORKERS_MAX;
    }
    printf("workers: %d\n", workers);

    /* 루트의 실제 위치를 한 번 구해 두고, 요청 파일이 이 안에 있는지 비교할 때 쓴다 */
    char root_real[PATH_MAX];
    if (realpath(cfg.root, root_real) == NULL) {
        perror("realpath");
        return EXIT_FAILURE;
    }

    /* 끊긴 클라이언트에 write()해도 서버가 죽지 않고 -1(EPIPE)을 받게 함 */
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = server_listen(cfg.port);
    if (listen_fd < 0)
        return EXIT_FAILURE;

    printf("listening on %d\n", cfg.port);
    fflush(stdout);

    server_run(listen_fd, root_real, rp, workers);
    return EXIT_SUCCESS;   /* 도달하지 않음 */
}
