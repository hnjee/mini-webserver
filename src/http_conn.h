#ifndef HTTP_CONN_H
#define HTTP_CONN_H

typedef enum {
    HTTP_CONN_RESPONDED = 0,   /* 응답을 끝까지 보냈다 */
    HTTP_CONN_PEER_CLOSED = 1, /* 요청 머리가 완성되기 전에 상대가 닫았다. 응답하지 않았다 */
    HTTP_CONN_ERROR = -1       /* 읽기·쓰기 오류. errno 보존 */
} http_conn_result;

/*
 * 연결 하나에서 요청 하나를 읽어 판정하고 응답 하나를 보낸 뒤 돌아간다(D-19, D-30).
 * root는 main이 realpath로 정규화한 절대 경로다. main이 소유하고 http_conn은 빌려 쓰기만 한다.
 * 머리 뒤에 남은 바이트와 POST 본문은 읽지 않는다. fd는 닫지 않는다(main이 닫는다).
 * 돌아갈 때 이 요청에서 연 파일 fd는 모두 닫혀 있다(D-33).
 */
http_conn_result http_conn_serve(int fd, const char *root);

#endif /* HTTP_CONN_H */
