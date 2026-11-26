#ifndef CONFIG_H
#define CONFIG_H

/* Step 0 설계 4장: 실행 인자로 받은 서버 설정 */
struct server_config {
    int         port;         /* 1~65535 */
    const char *root;         /* 정적 파일 루트, argv를 그대로 가리킴 */
    const char *config_path;  /* -c 값, 없으면 NULL */
    int         threads;      /* -t 값 1~256, 없으면 0 (main이 코어 수 × 2로 정함) */
};

/* Step 0 설계 5장: config_parse()의 결과 코드 */
enum config_result {
    CONFIG_OK = 0,
    CONFIG_ERR_USAGE,  /* 필수 인자 빠짐, 모르는 인자 */
    CONFIG_ERR_PORT,   /* 포트가 정수가 아니거나 1~65535 밖 */
    CONFIG_ERR_ROOT,   /* 루트가 없거나 디렉터리가 아님 */
    CONFIG_ERR_THREADS /* -t가 정수가 아니거나 1~256 밖 (Step 6) */
};

/* argv에서 -p, -r, -c, -t를 꺼내 out->port, out->root, out->config_path, out->threads에 채운다 */
enum config_result config_parse(int argc, char *argv[], struct server_config *out);

/* 사용법을 표준 에러에 출력한다 */
void config_print_usage(const char *prog);

#endif /* CONFIG_H */
