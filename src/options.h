#ifndef OPTIONS_H
#define OPTIONS_H

#include <stddef.h>

/* 실행 인자 파싱 결과. Step 0에서는 값을 확인만 하고 소켓은 열지 않는다. */
typedef struct {
    int port;         /* -p: 1~65535 */
    const char *root; /* -r: 존재하는 디렉터리 */
} options;

typedef enum {
    OPTIONS_OK = 0,
    OPTIONS_ERROR = 1, /* 인자가 잘못됐다 */
    OPTIONS_HELP = 2   /* -h: 사용법만 출력하고 끝낸다 */
} options_status;

/*
 * argv를 파싱해 out에 채운다. 실패하면 error에 사람이 읽는 이유를 쓴다.
 * getopt를 쓰므로 호출할 때마다 glibc getopt 상태를 초기화한다(optind = 0, 반복 호출 가능).
 * 라우팅 설정 파일 인자(-c)는 Step 6에서 추가한다. 지금은 받지 않는다.
 */
options_status options_parse(int argc, char *const argv[], options *out,
                             char *error, size_t error_size);

/* 사용법을 stream(FILE *)에 출력한다. */
void options_usage(void *stream, const char *program_name);

#endif /* OPTIONS_H */
