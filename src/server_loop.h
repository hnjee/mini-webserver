#ifndef SERVER_LOOP_H
#define SERVER_LOOP_H

#include <stddef.h>

/*
 * D-56, D-57: 듣는 소켓으로 연결을 받아 epoll(레벨 트리거)로 끝없이 처리한다.
 * 서버를 끝내는 실패(epoll 만들기·기다리기, D-07 밖 accept 오류)에서만 -1을 돌려주고 error에 이유를 쓴다.
 * listen_fd는 논블로킹으로 바꾼다. root는 main이 정규화한 루트이고 빌려 쓰기만 한다.
 */
int server_loop_run(int listen_fd, const char *root, char *error, size_t error_size);

#endif /* SERVER_LOOP_H */
