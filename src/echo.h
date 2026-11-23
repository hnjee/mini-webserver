#ifndef ECHO_H
#define ECHO_H

/* 한 번에 읽는 크기. 4 KB(일반적인 페이지 크기)면 에코에 충분하고 스택에 둬도 부담이 없다. */
#define ECHO_BUFFER_SIZE 4096

typedef enum {
    ECHO_PEER_CLOSED = 0, /* 상대가 쓰기를 닫았다(read == 0) */
    ECHO_ERROR = -1       /* 읽기·쓰기 오류. errno를 보존한다 */
} echo_result;

/*
 * 연결 fd에서 읽은 바이트를 줄 구분 없이 그대로 돌려준다. 상대가 닫거나 오류가 날 때까지 반복한다.
 * fd는 닫지 않는다(연결 fd는 accept한 main이 닫는다).
 */
echo_result echo_serve(int fd);

#endif /* ECHO_H */
