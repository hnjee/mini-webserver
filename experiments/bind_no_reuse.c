/*
 * D-06 실험: SO_REUSEADDR 없이 0.0.0.0:8080에 bind만 해 본다.
 * 서버가 먼저 끊은 연결이 TIME_WAIT 등으로 남아 있으면 EADDRINUSE로 실패하는 것을 보이려는 프로그램이다.
 * 서버 소스와 분리해 두며, bind 결과만 출력하고 끝난다(listen·accept 없음).
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 8080 /* TC-1-03은 서버와 같은 포트로 비교한다 */

int main(void)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return 2;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int err = errno;
        printf("bind 실패 (SO_REUSEADDR 없음, 포트 %d): errno=%d%s: %s\n", PORT, err,
               err == EADDRINUSE ? " (EADDRINUSE)" : "", strerror(err));
        close(fd);
        return 1;
    }
    printf("bind 성공 (SO_REUSEADDR 없음, 포트 %d)\n", PORT);
    close(fd);
    return 0;
}
