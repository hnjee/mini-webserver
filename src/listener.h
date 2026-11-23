#ifndef LISTENER_H
#define LISTENER_H

#include <stddef.h>

/*
 * 0.0.0.0:port에서 듣는 TCP 소켓을 연다(socket → SO_REUSEADDR → bind → listen).
 * 성공하면 소켓 fd를 돌려준다. 실패하면 자신이 연 소켓을 닫고 -1을 돌려주며,
 * error에 사람이 읽는 이유를 쓴다. 메시지 출력은 호출한 쪽(main)이 한다.
 */
int listener_open(int port, char *error, size_t error_size);

#endif /* LISTENER_H */
