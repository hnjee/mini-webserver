#ifndef OPTIONS_H
#define OPTIONS_H

#include <stddef.h>

/* 실행 인자 파싱 결과. Step 1은 -p로 소켓을 열고, -r은 존재만 확인한다(사용은 Step 3부터). */
typedef struct {
    int port;         /* -p: 1~65535 */
    const char *root; /* -r: 존재하는 디렉터리 (Step 3부터 사용) */
    const char *config; /* -c: 라우팅 설정 파일, 없으면 NULL(D-61). 내용 검사는 route_load */
} options;

typedef enum {
    OPTIONS_OK = 0,
    OPTIONS_ERROR = 1, /* 인자가 잘못됐다 */
    OPTIONS_HELP = 2   /* -h: 사용법만 출력하고 끝낸다 */
} options_status;

/*
 * argv를 파싱해 out에 채운다. 실패하면 error에 사람이 읽는 이유를 쓴다.
 * getopt를 쓰므로 호출할 때마다 glibc getopt 상태를 초기화한다(optind = 0, 반복 호출 가능).
 * -c(라우팅 설정 파일)는 선택 인자다(D-61).
 */
options_status options_parse(int argc, char *const argv[], options *out,
                             char *error, size_t error_size);

/* 사용법을 stream(FILE *)에 출력한다. */
void options_usage(void *stream, const char *program_name);

#endif /* OPTIONS_H */
