#ifndef SERVER_H
#define SERVER_H

/*
 * port에서 연결을 기다리는 소켓(listen_fd)을 연다.
 * socket() → setsockopt(SO_REUSEADDR) → bind() → listen() 순서로 한다.
 * 성공하면 listen_fd, 실패하면 -1을 돌려준다. 실패 이유는 stderr에 출력한다.
 */
int server_open(int port);

/*
 * accept()를 반복하며 연결을 하나씩 처리한다. 돌아오지 않는다.
 * Step 1에서는 받은 바이트를 그대로 돌려주는 에코 처리를 한다.
 */
void server_run(int listen_fd);

#endif
