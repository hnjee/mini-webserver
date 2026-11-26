#ifndef PROXY_H
#define PROXY_H

/* 리버스 프록시: docs/design/step6.md 4장 (S6-03 ~ S6-10) */

#include "route.h"
#include "server.h"

/* 이 연결이 프록시 중이면 1 */
int proxy_active(const struct conn *c);

/* 요청을 다 모은 뒤 라우팅 표가 맞으면 불러요. 백엔드를 고르고 연결을 시작해요 */
void proxy_start(struct server *s, struct conn *c, struct route *rt);

/* 백엔드 소켓에 알림이 왔을 때 */
void proxy_on_backend(struct server *s, struct conn *c, unsigned events);

/* 프록시 중인 연결의 클라이언트 소켓에 알림이 왔을 때 */
void proxy_on_client(struct server *s, struct conn *c, unsigned events);

/* deadline이 지났으면 504를 보내거나 닫아요. 무언가 했으면 1, 아니면 0 */
int proxy_check_timeout(struct server *s, struct conn *c, time_t now);

/* 백엔드 소켓을 epoll에서 빼고 닫고, 중계 버퍼를 해제해요. 여러 번 불러도 돼요 */
void proxy_cleanup(struct server *s, struct conn *c);

#endif
