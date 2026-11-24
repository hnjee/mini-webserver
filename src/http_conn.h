#ifndef HTTP_CONN_H
#define HTTP_CONN_H

typedef enum {
    HTTP_CONN_DONE = 0,        /* 요청 사이에 상대가 닫음·유휴 타임아웃, 닫는 응답과 정리 끝 */
    HTTP_CONN_PEER_CLOSED = 1, /* 요청 도중(머리·본문) 상대가 닫았다 */
    HTTP_CONN_TIMED_OUT = 2,   /* 요청 도중 타임아웃 */
    HTTP_CONN_ERROR = -1       /* 읽기·쓰기·소켓 설정 실패. errno 보존 */
} http_conn_result;

/*
 * 연결 하나의 요청을 반복 처리한다(D-46): 타임아웃 설정, 버퍼·남은 바이트, 판정, 분기, 유지 판단, 닫기 전 정리.
 * root는 main이 realpath로 정규화한 절대 경로다. main이 소유하고 http_conn은 빌려 쓰기만 한다.
 * fd는 닫지 않는다(main이 닫는다). 돌아갈 때 이 연결에서 연 파일 fd는 모두 닫혀 있다(D-29).
 */
http_conn_result http_conn_serve(int fd, const char *root);

#endif /* HTTP_CONN_H */
