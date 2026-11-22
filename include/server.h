#ifndef SERVER_H
#define SERVER_H

/* 리슨 소켓을 준비해요. 성공하면 fd, 실패하면 -1 (이유 출력, 소켓 닫음) */
int server_listen(int port);

/* accept → serve_client → close를 끝없이 반복해요. 돌아오지 않아요. */
void server_run(int listen_fd);

/* 클라이언트가 끊을 때까지 에코해요. 끊으면 0, 실패하면 -1. 소켓은 닫지 않아요. */
int serve_client(int client_fd);

#endif
