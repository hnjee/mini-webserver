#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>

/* config_parse()의 반환값 */
enum config_result {
    CONFIG_OK = 0,     /* 설정을 모두 읽었다 */
    CONFIG_HELP = 1,   /* -h: 사용법만 출력하고 끝낸다 */
    CONFIG_ERROR = -1  /* 잘못된 인자. 이유는 stderr에 이미 출력했다 */
};

/* 실행 인자로 받은 서버 설정 */
struct server_config {
    int port;                /* -p: 서버가 열 포트 번호 (1~65535) */
    const char *root;        /* -r: 정적 파일 루트 디렉터리 */
    const char *routes_path; /* -c: 리버스 프록시 라우팅 설정 파일. 없으면 NULL */
};

/*
 * argv에서 -p, -r, -c 옵션을 읽어 cfg를 채운다.
 * 옵션 순서는 상관없다. -p와 -r은 필수, -c는 선택이다.
 * cfg의 문자열은 argv를 가리키므로 따로 해제하지 않는다.
 */
enum config_result config_parse(int argc, char *argv[], struct server_config *cfg);

/* 사용법을 out에 출력한다. */
void config_print_usage(FILE *out, const char *prog);

#endif
