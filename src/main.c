#include "config.h"

#include <stdio.h>
#include <stdlib.h>

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
    }

    printf("port: %d\n", cfg.port);
    printf("root: %s\n", cfg.root);
    if (cfg.config_path != NULL) {
        printf("config: %s\n", cfg.config_path);
    }

    /* 소켓은 Step 1에서 연다 */
    return EXIT_SUCCESS;
}
