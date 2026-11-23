#include "server.h"

#include "request.h"
#include "response.h"
#include "static.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* POST에 보내는 고정 본문. POST가 할 일은 Step 4에서 정한다 */
static const char HELLO_BODY[] = "<h1>Hello, world!</h1>";

/*
 * 연결 하나에서 HTTP 요청 하나를 처리하고 client_fd를 닫는다.
 *   1. 버퍼에 이어 붙이며 "\r\n\r\n"이 나올 때까지 read()를 반복한다
 *      (한 번의 read()로 요청이 다 온다고 가정하지 않는다)
 *   2. 헤더를 파싱해 400·501·505를 판정한다
 *   3. GET이면 root 안의 파일을 보내고, POST면 고정 HTML로 200, 에러면 에러 응답을 보낸다
 * 헤더 뒤에 딸려 온 바이트는 아직 버린다 (Step 4에서 본문이나 다음 요청으로 쓴다).
 */
static void handle_http(int client_fd, const char *root)
{
    char buf[REQUEST_BUF_SIZE];
    size_t len = 0;
    ssize_t end;
    struct http_request req;
    int status;
    int rc = 0;

    /* 1. 헤더 끝까지 읽기 */
    for (;;) {
        end = request_find_end(buf, len);
        if (end >= 0)
            break;
        if (len == sizeof(buf)) {
            /* 버퍼가 가득 찼는데 헤더가 끝나지 않았다 */
            if (send_error(client_fd, 431) < 0)
                perror("write");
            printf("request:   (header too large) -> 431\n");
            close(client_fd);
            return;
        }

        ssize_t n = read(client_fd, buf + len, sizeof(buf) - len);
        if (n > 0) {
            len += (size_t)n;
        } else if (n == 0) {
            /* 요청을 다 보내기 전에 상대가 끊었다. 응답할 대상이 없다 */
            close(client_fd);
            return;
        } else {
            if (errno == EINTR)
                continue;
            perror("read");
            close(client_fd);
            return;
        }
    }

    /* 2. 파싱과 판정. req의 문자열은 buf 안을 가리키므로 응답까지 buf를 건드리지 않는다 */
    status = parse_request(buf, (size_t)end, &req);

    /* 3. 응답 */
    if (status == 0 && strcmp(req.method, "GET") == 0) {
        status = serve_static(client_fd, root, &req);   /* 응답까지 보낸다 */
    } else if (status == 0) {
        status = 200;
        rc = send_response(client_fd, status, "text/html",
                           HELLO_BODY, sizeof(HELLO_BODY) - 1);
    } else {
        rc = send_error(client_fd, status);
    }
    if (rc < 0)
        perror("write");

    printf("request:   %s %s -> %d\n",
           req.method ? req.method : "-", req.uri ? req.uri : "-", status);
    close(client_fd);
}

int server_open(int port)
{
    struct sockaddr_in addr;
    int listen_fd;
    int on = 1;

    /* 1. 소켓 만들기: 커널이 소켓을 만들고 fd 번호를 돌려준다 */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    /* 2. TIME_WAIT가 남아 있어도 bind()할 수 있게 한다. bind()보다 먼저 해야 한다 */
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(listen_fd);
        return -1;
    }

    /* 3. 포트에 묶기: 모든 주소(INADDR_ANY)에서, 네트워크 바이트 순서로 */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    /* 4. 연결 요청을 받기 시작한다. handshake가 끝난 연결은 accept 대기열에 쌓인다 */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

void server_run(int listen_fd, const char *root)
{
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        char ip[INET_ADDRSTRLEN];
        int client_fd;
        int client_port;

        /* 5. accept 대기열에서 연결 하나를 꺼낸다. 연결마다 새 fd가 생긴다 */
        client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;   /* 연결 하나의 실패로 서버를 멈추지 않는다 */
        }

        if (inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip)) == NULL)
            strcpy(ip, "?");
        client_port = ntohs(client_addr.sin_port);

        printf("connected: %s:%d (fd=%d)\n", ip, client_port, client_fd);
        handle_http(client_fd, root);   /* 끝나면 client_fd를 닫고 돌아온다 */
        printf("closed:    %s:%d\n", ip, client_port);
    }
}
