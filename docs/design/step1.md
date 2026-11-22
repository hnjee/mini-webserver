# Step 1 설계: TCP 에코 서버

이 문서를 읽으면 Step 1 서버가 리슨 소켓을 어떻게 준비하고, 클라이언트를 어떻게 받아 받은 바이트를 그대로 돌려주는지 알 수 있어요. 함수별 동작 순서와 시나리오별 실행 순서를 그대로 따라 `src/server.c`와 `src/main.c`를 구현할 수 있어요.

요구사항은 [Step 1 요구사항](../requirements/step1.md)을 기준으로 해요.

## 전체 흐름

```
./webserver -p 8080 -r ./www
  → main.c    : config_parse() 인자 검사
  → server.c  : server_listen()  socket → setsockopt → bind → listen
  → main.c    : "listening on 8080" 출력
  → server.c  : server_run()     accept → serve_client() → close → 다시 accept
                serve_client()   read → write(다 쓸 때까지) → read → … → read가 0이면 끝
```

- `main.c`는 종료를 결정해요. 준비 단계 함수가 `-1`을 돌려주면 종료 코드 1로 끝내요.
- `server_listen()`은 실패해도 종료하지 않아요. 이유를 출력하고, 연 소켓을 닫고, `-1`을 돌려줘요.
- `server_run()`은 돌아오지 않아요. 클라이언트 소켓은 여기서 한 번만 닫아요.
- `serve_client()`는 에코만 해요. 소켓을 닫지 않고, 어떻게 끝났는지만 돌려줘요.

## 자료구조

### fd

커널은 소켓 구조체를 커널 메모리에 두고, 서버에는 그 소켓을 가리키는 번호(fd)만 줘요. 서버가 다루는 소켓은 두 종류예요.

| 이름 | 타입 | 뜻 |
| --- | --- | --- |
| `listen_fd` | `int` | 리슨 소켓 번호예요. `server_listen()`이 만들고 서버가 끝날 때까지 열려 있어요. (예: `3`) |
| `client_fd` | `int` | 연결 하나를 담당하는 소켓 번호예요. `accept()`가 줄 때마다 새로 생기고, 그 연결이 끝나면 닫아요. (예: `4`) |

### read와 write

둘 다 서버(유저 프로그램) 메모리에 있는 `buf`(`serve_client()` 안의 `char buf[BUF_SIZE]`, `BUF_SIZE`는 4096)와 커널 메모리에 있는 소켓 버퍼 사이에서 데이터를 복사하고, 복사한 만큼의 바이트 수를 돌려줘요.

| 시스템 콜 | 복사 방향 | 돌려주는 값 |
| --- | --- | --- |
| `read(fd, buf, 4096)` | 커널 받기 버퍼 → `buf` | 가져온 바이트 수, `0`(끊김), `-1`(실패) |
| `write(fd, buf, n)` | `buf` → 커널 보내기 버퍼 | 복사한 바이트 수, `-1`(실패) |

둘 다 요청한 만큼이 아니라 지금 가능한 만큼만 복사하고 리턴할 수 있어요. 그래서 `read()`는 `0`이 나올 때까지, `write()`는 다 쓸 때까지 반복해요. (S1-20, S1-23)

## 리슨 소켓 준비: server_listen

포트 번호를 받아 리슨 소켓을 준비하고 그 fd를 돌려줘요. (S1-01 ~ S1-11)

| 돌려주는 값 | 뜻 |
| --- | --- |
| `3` 이상 | 성공이에요. LISTEN 상태인 리슨 소켓 번호예요. |
| `-1` | 실패예요. 이유는 이미 출력했고, 연 소켓도 이미 닫았어요. |

동작 순서

1. `socket(AF_INET, SOCK_STREAM, 0)`으로 IPv4·TCP 소켓을 만들어요.
2. `setsockopt()`로 `SO_REUSEADDR`을 켠 뒤, 0.0.0.0(이 컴퓨터의 모든 IP)과 포트를 `bind()`로 붙여요. (S1-05)
3. `listen(fd, SOMAXCONN)`으로 LISTEN 상태로 바꾸고 accept 큐를 만들어요.
4. fd를 돌려줘요.

어느 단계든 실패하면 이유를 출력하고, 연 소켓이 있으면 닫은 뒤 `-1`을 돌려줘요.

```c
int server_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}
```

## 클라이언트 받기: server_run

리슨 소켓에서 클라이언트를 하나씩 꺼내 에코하고 닫기를 끝없이 반복해요. 돌아오지 않아요. (S1-12 ~ S1-17, S1-27 ~ S1-32)

동작 순서

1. `accept(listen_fd, NULL, NULL)`로 accept 큐에서 연결 하나를 꺼내요. 큐가 비어 있으면 연결이 올 때까지 잠들어요.
2. `serve_client(client_fd)`로 에코해요.
3. `serve_client()`가 어떻게 끝났든 `close(client_fd)`로 한 번 닫아요.
4. 1번으로 돌아가요.

`accept()`가 실패해도 서버를 끝내지 않고 1번으로 돌아가요.

```c
void server_run(int listen_fd)
{
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }

        serve_client(client_fd);

        if (close(client_fd) < 0)
            perror("close");
    }
}
```

## 에코: serve_client

클라이언트 소켓에서 읽은 바이트를 그대로 돌려줘요. 클라이언트가 끊을 때까지 반복해요. 소켓은 닫지 않아요. (S1-18 ~ S1-26)

| 돌려주는 값 | 뜻 |
| --- | --- |
| `0` | 클라이언트가 연결을 끊었어요. (`read()`가 `0`) |
| `-1` | 읽기나 쓰기가 실패했어요. 이유는 이미 출력했어요. |

동작 순서

1. `read(client_fd, buf, BUF_SIZE)`로 받기 버퍼에 있는 만큼 가져와요. 비어 있으면 데이터가 올 때까지 잠들어요.
2. `0`이면 클라이언트가 끊은 거예요. `0`을 돌려주고 끝내요.
3. 양수 `n`이면 받은 `n`바이트를 다 쓸 때까지 `write()`를 반복해요. (S1-23)
4. 1번으로 돌아가요.

읽기나 쓰기가 실패하면 `-1`을 돌려줘요.

```c
int serve_client(int client_fd)
{
    char buf[BUF_SIZE];

    for (;;) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n == 0)
            return 0;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return -1;
        }

        ssize_t sent = 0;                       /* 받은 n바이트를 다 쓸 때까지 */
        while (sent < n) {
            ssize_t w = write(client_fd, buf + sent, (size_t)(n - sent));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                perror("write");
                return -1;
            }
            sent += w;
        }
    }
}
```

`read()`는 "다 오면" 리턴하는 게 아니라 1바이트라도 있으면 바로 리턴해요. 커널은 어디까지가 한 덩어리인지 모르므로, 끝은 `0`으로만 판단해요.

## main 연결

Step 0의 `main.c`에서 인자 출력 뒤에 아래를 붙여요. Step 0 출력(`port:`, `root:`)은 그대로 둬요.

동작 순서

1. `config_parse()`로 인자를 검사해요. (Step 0 그대로)
2. `signal(SIGPIPE, SIG_IGN)`으로 SIGPIPE를 무시해요. 끊긴 클라이언트에 `write()`해도 서버가 죽지 않고 `-1`(`EPIPE`)을 받아 그 연결만 닫아요. (S1-25)
3. `server_listen(cfg.port)`로 리슨 소켓을 준비해요. `-1`이면 `EXIT_FAILURE`로 끝내요.
4. `listening on 포트`를 출력하고 `fflush(stdout)`으로 바로 내보내요.
5. `server_run(listen_fd)`로 들어가요. 돌아오지 않아요.

```c
/* config_parse 성공, port/root 출력 뒤 */
signal(SIGPIPE, SIG_IGN);

int listen_fd = server_listen(cfg.port);
if (listen_fd < 0)
    return EXIT_FAILURE;

printf("listening on %d\n", cfg.port);
fflush(stdout);

server_run(listen_fd);
return EXIT_SUCCESS;   /* 도달하지 않음 */
```

## 시나리오별 실행 순서

### 서버 시작 (`./webserver -p 8080 -r ./www`)

1. `config_parse()`가 인자를 검사하고 `port: 8080`, `root: ./www`를 출력해요.
2. SIGPIPE를 무시하도록 설정해요.
3. `server_listen(8080)`이 socket → setsockopt → bind → listen을 차례로 불러요. 리슨 소켓은 fd 3번이에요. (S1-01, S1-08)
4. `listening on 8080`을 출력해요. (S1-10)
5. `server_run(3)`이 `accept()`를 불러요. accept 큐가 비어 있어서 잠들어요. (S1-13)

### 연결 하나 처리 (`echo hi | nc -N localhost 8080`)

1. 클라이언트가 연결하면 커널이 악수를 끝내고 연결을 accept 큐에 넣은 뒤, 자고 있던 `accept()`를 깨워요.
2. `accept()`가 `4`를 돌려줘요. (S1-12)
3. `serve_client(4)`의 `read()`가 `"hi\n"` 3바이트를 가져와요. (S1-18)
4. `write()`가 3바이트를 돌려줘요. 클라이언트 화면에 `hi`가 찍혀요. (S1-22)
5. `nc -N`이 입력을 다 보내고 FIN을 보내요. 다음 `read()`가 `0`을 돌려줘요.
6. `serve_client()`가 `0`을 돌려주고, `server_run()`이 `close(4)`로 닫아요. (S1-27)
7. 다시 `accept()`로 돌아가 잠들어요. (S1-14)

### 끊긴 클라이언트에 쓰기 (`head -c 50000000 /dev/zero | nc localhost 8080 | head -c 1`)

1. 서버가 받은 0바이트들을 계속 돌려줘요.
2. `head -c 1`이 1바이트를 받고 끝나면서 `nc`도 끝나요. 연결이 끊겨요.
3. 서버의 다음 `write()`가 `-1`(`EPIPE`)을 돌려줘요. SIGPIPE는 무시했으니 서버는 죽지 않아요.
4. `serve_client()`가 `write: Broken pipe`를 출력하고 `-1`을 돌려줘요.
5. `server_run()`이 `close(4)`로 닫고 다시 `accept()`로 돌아가요. (S1-25)

### 연결 두 개가 동시에 들어옴

1. 클라이언트 A가 접속해 `serve_client()`의 `read()`에서 데이터를 기다려요.
2. 클라이언트 B가 접속하면 커널이 악수를 끝내고 B를 accept 큐에 넣어요. 서버는 A의 `read()`에 묶여 있어서 B를 꺼내지 못해요.
3. B가 보낸 데이터는 B 소켓의 받기 버퍼에 쌓여 있어요.
4. A가 끊으면 `close(4)` 후 `accept()`가 B를 꺼내요. B는 다시 4번을 받아요.
5. `read()`가 받기 버퍼에 쌓여 있던 B의 데이터를 바로 가져와 돌려줘요. (S1-15)

## 파일 구성

| 파일 | 내용 | Step 1 변경 |
| --- | --- | --- |
| `include/server.h` | `server_listen`, `server_run`, `serve_client` 선언 | 새로 만듦 |
| `src/server.c` | 이 문서의 함수 구현 | 새로 만듦 |
| `src/main.c` | SIGPIPE 무시, 서버 시작 | 수정 |
| `tests/test_server.c` | 서버 함수 단위 테스트 | 새로 만듦 |
| `Makefile` | `src/*.c`를 자동으로 잡으므로 그대로 | 그대로 |

```c
/* include/server.h */
#ifndef SERVER_H
#define SERVER_H

/* 리슨 소켓을 준비해요. 성공하면 fd, 실패하면 -1 (이유 출력, 소켓 닫음) */
int server_listen(int port);

/* accept → serve_client → close를 끝없이 반복해요. 돌아오지 않아요. */
void server_run(int listen_fd);

/* 클라이언트가 끊을 때까지 에코해요. 끊으면 0, 실패하면 -1. 소켓은 닫지 않아요. */
int serve_client(int client_fd);

#endif
```

`server.c`에 필요한 헤더는 `<arpa/inet.h>`, `<errno.h>`, `<netinet/in.h>`, `<stdio.h>`, `<string.h>`, `<sys/socket.h>`, `<unistd.h>`예요. `main.c`에는 `<signal.h>`와 `"server.h"`를 더해요.

## 테스트 방법

| 대상 | 방법 | 근거 |
| --- | --- | --- |
| `server_listen()` | `server_listen(0)`으로 빈 포트에 열고, `getsockopt(SO_ACCEPTCONN)`이 1인지, `SO_REUSEADDR`이 1인지 봐요. `getsockname()`으로 받은 포트로 한 번 더 부르면 `-1`인지 봐요. | S1-03, S1-06, S1-09 |
| `serve_client()` 에코 | `socketpair()`로 연결된 소켓 두 개를 만들어 한쪽을 넘겨요. 다른 쪽에 `hi`를 쓰고 `shutdown(SHUT_WR)` 한 뒤 읽어서 같은지, 리턴값이 `0`인지 봐요. | S1-18, S1-22 |
| `serve_client()` 큰 데이터 | `fork()`로 자식은 1MB를 쓰고, 부모는 읽어서 같은지 비교해요. 한쪽이 막혀도 다른 쪽이 비워 줘서 멈추지 않아요. | S1-20, S1-23 |
| 서버 전체 | 요구사항 표의 확인 명령어를 그대로 실행하고 결과를 테스트 문서에 적어요. | S1-01 ~ S1-31 |
| 오류 경로 | `EINTR` 재시도, `perror`, `close` 한 번, `exit` 없음을 코드 리뷰 체크리스트로 봐요. | S1-04, S1-11, S1-16, S1-17, S1-21, S1-26, S1-32 |
