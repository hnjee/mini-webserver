/* Step 0 단위 테스트: 실행 인자 검사. assert 기반이라 외부 도구가 필요 없다. */
#define _POSIX_C_SOURCE 200809L

#include "../src/options.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *name, int condition, const char *detail)
{
    if (condition) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (%s)\n", name, detail);
        failures++;
    }
}

static options_status run(char *const argv[], int argc, options *out, char *error, size_t size)
{
    return options_parse(argc, argv, out, error, size);
}

int main(void)
{
    options config;
    char error[256];
    options_status status;

    printf("test_options\n");

    {
        char *argv[] = {"webserver", "-p", "8080", "-r", ".", NULL};
        status = run(argv, 5, &config, error, sizeof(error));
        check("유효한 인자", status == OPTIONS_OK, error);
        check("포트 값", config.port == 8080, "포트가 8080이 아니다");
        check("루트 값", config.root != NULL && strcmp(config.root, ".") == 0, "루트가 다르다");
    }
    {
        /* TC-6-06: -c는 선택 인자다(D-61). 파일 내용 검사는 main(route_load)이 한다. */
        char *argv[] = {"webserver", "-p", "8080", "-r", ".", "-c", "./routes.conf", NULL};
        status = run(argv, 7, &config, error, sizeof(error));
        check("TC-6-06 -c 받음", status == OPTIONS_OK && config.config != NULL &&
                                   strcmp(config.config, "./routes.conf") == 0, error);
    }
    {
        char *argv[] = {"webserver", "-p", "8080", "-r", ".", NULL};
        status = run(argv, 5, &config, error, sizeof(error));
        check("TC-6-06 -c 생략이면 config NULL", status == OPTIONS_OK && config.config == NULL, "NULL이 아니다");
    }
    {
        char *argv[] = {"webserver", "-p", "8080", "-r", ".", "-c", NULL};
        check("TC-6-06 -c 값 없음 거부", run(argv, 6, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-p", "1", "-r", ".", NULL};
        check("포트 하한 1", run(argv, 5, &config, error, sizeof(error)) == OPTIONS_OK, error);
    }
    {
        char *argv[] = {"webserver", "-p", "65535", "-r", ".", NULL};
        check("포트 상한 65535", run(argv, 5, &config, error, sizeof(error)) == OPTIONS_OK, error);
    }
    {
        char *argv[] = {"webserver", "-p", "0", "-r", ".", NULL};
        check("포트 0 거부", run(argv, 5, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-p", "65536", "-r", ".", NULL};
        check("포트 65536 거부", run(argv, 5, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-p", "80x", "-r", ".", NULL};
        check("정수가 아닌 포트 거부", run(argv, 5, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-p", "8080", "-r", "./no-such-dir-step0", NULL};
        check("없는 루트 거부", run(argv, 5, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-p", "8080", "-r", "Makefile", NULL};
        check("파일 경로 루트 거부", run(argv, 5, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-r", ".", NULL};
        check("-p 누락 거부", run(argv, 3, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-p", "8080", NULL};
        check("-r 누락 거부", run(argv, 3, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-p", NULL};
        check("-p 값 누락 거부", run(argv, 2, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-z", NULL};
        check("알 수 없는 인자 거부", run(argv, 2, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-p", "8080", "-r", ".", "extra", NULL};
        check("남는 인자 거부", run(argv, 6, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
    }
    {
        char *argv[] = {"webserver", "-h", NULL};
        check("-h는 도움말", run(argv, 2, &config, error, sizeof(error)) == OPTIONS_HELP, "도움말이 아니다");
    }
    {
        char *argv[] = {"webserver", "-p", "0", "-r", ".", NULL};
        run(argv, 5, &config, error, sizeof(error));
        check("실패 이유 메시지", error[0] != '\0', "메시지가 비었다");
    }
    {
        /* 묶음 옵션 중간에서 조기 반환하면 getopt에 잔여 문자가 남는다. 다음 호출은 그 영향을 받으면 안 된다. */
        char *bad[] = {"webserver", "-zx", NULL};
        char *valid[] = {"webserver", "-p", "8080", "-r", ".", NULL};
        check("-zx 거부", run(bad, 2, &config, error, sizeof(error)) == OPTIONS_ERROR, "허용됐다");
        status = run(valid, 5, &config, error, sizeof(error));
        check("-zx 오류 뒤 정상 재파싱", status == OPTIONS_OK && config.port == 8080, error);
    }
    {
        char *help[] = {"webserver", "-hp", NULL};
        char *valid[] = {"webserver", "-p", "8080", "-r", ".", NULL};
        check("-hp는 도움말", run(help, 2, &config, error, sizeof(error)) == OPTIONS_HELP, "도움말이 아니다");
        status = run(valid, 5, &config, error, sizeof(error));
        check("-hp 도움말 뒤 정상 재파싱", status == OPTIONS_OK && config.port == 8080, error);
    }

    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
