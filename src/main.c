#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "server.h"

int main(int argc, char *argv[])
{
    struct server_config cfg;
    char root[PATH_MAX];
    int listen_fd;

    switch (config_parse(argc, argv, &cfg)) {
    case CONFIG_HELP:
        config_print_usage(stdout, argv[0]);
        return 0;
    case CONFIG_ERROR:
        config_print_usage(stderr, argv[0]);
        return 2;
    case CONFIG_OK:
        break;
    }

    /* 로그를 줄마다 바로 출력한다 (파이프나 파일로 보내도 쌓아두지 않게) */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 루트의 실제 절대 경로를 한 번 구해 둔다. 요청 경로가 이 안인지 비교할 때 쓴다 */
    if (realpath(cfg.root, root) == NULL) {
        perror("realpath(root)");
        return 1;
    }

    listen_fd = server_open(cfg.port);
    if (listen_fd < 0)
        return 1;

    printf("listening on port %d (root: %s)\n", cfg.port, root);
    server_run(listen_fd, root);   /* 돌아오지 않는다 */

    close(listen_fd);
    return 0;
}
