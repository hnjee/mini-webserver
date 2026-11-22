# Step 1 요구사항: TCP 에코 서버

Step 1 서버는 리슨 소켓 하나로 클라이언트를 한 명씩 받아, 받은 바이트를 그대로 돌려줘요. 준비 단계(socket, setsockopt, bind, listen)에서 실패하면 종료 코드 1로 끝나고, 클라이언트 단계(accept, read, write, close)에서 실패하면 그 클라이언트만 정리하고 계속 떠 있어요.

## 요구사항

| ID | 유형 | 요구사항 | 확인 명령어 | 기대 결과 |
| --- | --- | --- | --- | --- |
| S1-01 | socket | 서버는 시작할 때 `socket()`으로 IPv4·TCP 리슨 소켓 하나만 만들어야 한다. | 서버를 띄운 채 `ls -l /proc/$(pgrep -n webserver)/fd \| grep -c socket` | `1` (0, 1, 2 다음 번호인 3번에 `socket:[숫자]` 줄이 있다) |
| S1-02 | socket | `socket()`이 실패하면(fd가 0 미만이면), 서버는 이유를 출력하고 종료 코드 1로 끝나야 한다. | `strace -o /dev/null -e inject=socket:error=EMFILE ./webserver -p 8081 -r ./www; echo "exit=$?"` | `socket: Too many open files`가 출력되고, `exit=1`이 나오고, `ss -ltn \| grep 8081`에 아무것도 없다 |
| S1-03 | setsockopt | 서버는 `bind()` 전에 리슨 소켓에 `SO_REUSEADDR`을 켜서, 서버를 끈 뒤 1분 안에 다시 켜도 `bind()`가 실패하지 않아야 한다. | 터미널 1: 서버 실행<br>터미널 2: `nc localhost 8080` (접속 유지)<br>터미널 1: Ctrl+C → `ss -tan \| grep 8080` → 바로 서버 다시 실행 | `TIME-WAIT` 줄이 보이는데도 `listening on 8080`이 나온다 |
| S1-04 | setsockopt | `setsockopt()`가 실패하면, 서버는 이유를 출력하고 소켓을 닫은 뒤 종료 코드 1로 끝나야 한다. | 코드 리뷰 | 실패 경로에 `perror("setsockopt")`와 `close(fd)`가 있고 종료 코드 1로 끝난다 |
| S1-05 | bind | 서버는 리슨 소켓에 모든 IP 주소와 `-p`로 받은 포트 번호를 붙여야 한다. | 서버를 띄운 채 `ss -ltn \| grep 8080` | `0.0.0.0:8080`이 나온다 |
| S1-06 | bind | 포트를 이미 다른 프로그램이 쓰고 있으면, 서버는 이유를 출력하고 리슨 소켓을 닫은 뒤 종료 코드 1로 끝나야 한다. | 서버를 하나 띄운 채 다른 터미널에서 `./webserver -p 8080 -r ./www; echo "exit=$?"` | `bind: Address already in use`, `exit=1` |
| S1-07 | bind | 1024 미만 포트를 쓸 권한이 없으면, 서버는 이유를 출력하고 리슨 소켓을 닫은 뒤 종료 코드 1로 끝나야 한다. | 일반 사용자로 `./webserver -p 80 -r ./www; echo "exit=$?"` | `bind: Permission denied`, `exit=1` |
| S1-08 | listen | 서버는 리슨 소켓을 socket → setsockopt → bind → listen 순서로 준비해야 한다. | `strace -e trace=socket,setsockopt,bind,listen ./webserver -p 8080 -r ./www` | 네 시스템 콜이 이 순서로 찍히고 모두 성공한다 |
| S1-09 | listen | 서버는 리슨 소켓을 LISTEN 상태로 만들어 클라이언트 연결을 받을 준비를 해야 한다. | 서버를 띄운 채 `ss -ltn \| grep 8080` | 줄 맨 앞이 `LISTEN`이다 |
| S1-10 | listen | 준비가 끝나면, 서버는 `listening on 포트`를 출력하고 끝나지 않고 클라이언트를 기다려야 한다. | `./webserver -p 8080 -r ./www` | `listening on 8080`이 나오고 프롬프트로 돌아오지 않는다 |
| S1-11 | listen | `listen()`이 실패하면, 서버는 이유를 출력하고 리슨 소켓을 닫은 뒤 종료 코드 1로 끝나야 한다. | 코드 리뷰 | 실패 경로에 `perror("listen")`과 `close(fd)`가 있고 종료 코드 1로 끝난다 |
| S1-12 | accept | 클라이언트가 서버에 연결하면, 서버는 `accept()`로 accept 큐에서 그 연결을 꺼내 클라이언트 소켓 fd를 얻어야 한다. | `nc localhost 8080 &` 로 접속해 둔 뒤 `ls -l /proc/$(pgrep -n webserver)/fd \| grep -c socket` | `2` |
| S1-13 | accept | accept 큐가 비어 있는 동안, 서버는 `accept()`에서 블로킹으로 기다리며 CPU를 쓰지 않아야 한다. | 클라이언트 없이 서버를 띄운 채 `ps -o stat,pcpu -p $(pgrep -n webserver)` | `STAT`이 `S`, `%CPU`가 `0.0` |
| S1-14 | accept | 클라이언트 소켓을 닫으면, 서버는 다시 `accept()`로 돌아가 다음 연결을 꺼내야 한다. | `echo a \| nc -N localhost 8080`<br>`echo b \| nc -N localhost 8080` | `a`, `b`가 차례로 돌아오고 서버는 계속 떠 있다 |
| S1-15 | accept | 한 클라이언트를 처리하는 동안, 다음 연결은 accept 큐에 남아 있다가 앞 클라이언트 소켓을 닫은 뒤 꺼내져야 한다. (Step 5 전까지의 한계) | 터미널 2: `nc localhost 8080` 유지<br>터미널 3: `echo hi \| nc -N localhost 8080` | 터미널 3은 터미널 2를 끝낼 때까지 `hi`를 못 받고, 끝내면 바로 받는다 |
| S1-16 | accept | `accept()`가 시그널 때문에 중단되면(`EINTR`), 서버는 다시 `accept()`를 불러야 한다. | 코드 리뷰 | `errno == EINTR`이면 다시 부르는 코드가 있다 |
| S1-17 | accept | `accept()`가 그 밖의 이유로 실패하면, 서버는 이유를 출력하고 종료하지 않은 채 다음 `accept()`로 넘어가야 한다. | 코드 리뷰 | 실패 경로에 `perror("accept")`가 있고 `exit`나 `return`이 없다 |
| S1-18 | read | 클라이언트가 데이터를 보내면, 서버는 `read()`로 클라이언트 소켓에서 그 데이터를 읽어야 한다. | `echo hi \| nc -N localhost 8080` | `hi` |
| S1-19 | read | 받을 데이터가 없는 동안, 서버는 `read()`에서 블로킹으로 기다리며 CPU를 쓰지 않아야 한다. | `nc localhost 8080`으로 접속만 해 둔 채<br>`ps -o stat,pcpu -p $(pgrep -n webserver)` | `STAT`이 `S`, `%CPU`가 `0.0` |
| S1-20 | read | 서버는 데이터가 한 번에 다 온다고 가정하지 않고, `read()`가 `0`을 리턴할 때까지 반복해서 읽어야 한다. | `head -c 1000000 /dev/urandom > /tmp/in`<br>`nc -N localhost 8080 < /tmp/in > /tmp/out`<br>`cmp /tmp/in /tmp/out && echo same` | `same` |
| S1-21 | read | `read()`가 시그널 때문에 중단되면(`EINTR`), 서버는 다시 `read()`를 불러야 한다. | 코드 리뷰 | `errno == EINTR`이면 다시 부르는 코드가 있다 |
| S1-22 | write | `read()`로 데이터를 받으면, 서버는 받은 바이트를 그대로 `write()`로 클라이언트에게 돌려줘야 한다. | `printf 'a b\nc' \| nc -N localhost 8080 \| od -c` | 보낸 것과 같은 `a b \n c` |
| S1-23 | write | 서버는 `write()`가 일부만 쓰면 남은 바이트를 이어서 써서, 받은 바이트를 모두 보내야 한다. | S1-20의 1MB `cmp` 테스트 | `same` |
| S1-24 | write | 클라이언트가 읽지 않아 보내기 버퍼가 꽉 찬 동안, 서버는 `write()`에서 블로킹으로 기다리며 CPU를 쓰지 않아야 한다. | `(head -c 50000000 /dev/zero; sleep 30) \| nc localhost 8080 \| sleep 30 &`<br>몇 초 뒤 `ps -o stat,pcpu -p $(pgrep -n webserver)` | `STAT`이 `S`, `%CPU`가 `0.0` |
| S1-25 | write | 클라이언트가 이미 끊은 소켓에 쓰면(`EPIPE`), 서버는 종료되지 않고 그 클라이언트 소켓만 닫아야 한다. | `head -c 50000000 /dev/zero \| nc localhost 8080 \| head -c 1`<br>`echo hi \| nc -N localhost 8080` | 서버는 계속 떠 있고 `hi`가 돌아온다 |
| S1-26 | write | `write()`가 시그널 때문에 중단되면(`EINTR`), 서버는 다시 `write()`를 불러야 한다. | 코드 리뷰 | `errno == EINTR`이면 다시 부르는 코드가 있다 |
| S1-27 | close | 클라이언트 처리가 끝나면(`read()`가 `0`이거나 `read()`·`write()`가 실패하면), 서버는 이유가 있으면 출력하고 `close()`로 그 클라이언트 소켓만 닫은 뒤 다음 `accept()`로 넘어가야 한다. | `nc localhost 8080` 접속 후 Ctrl+C로 끊고<br>`ls -l /proc/$(pgrep -n webserver)/fd \| grep -c socket` | `1` |
| S1-28 | close | 서버가 클라이언트 소켓을 닫으면, 클라이언트는 연결이 끝난 것을 알고 종료할 수 있어야 한다. | `echo hi \| nc -N localhost 8080; echo "exit=$?"` | `hi` 다음 `exit=0`이 바로 찍힌다 |
| S1-29 | close | 서버는 동시에 열린 소켓을 리슨 소켓 1개와 클라이언트 소켓 1개, 최대 2개로 유지해야 한다. | `for i in $(seq 200); do echo hi \| nc -N localhost 8080 > /dev/null; done`<br>`ls -l /proc/$(pgrep -n webserver)/fd \| grep -c socket` | `1` |
| S1-30 | close | 서버는 클라이언트 소켓을 닫은 뒤에도 리슨 소켓은 닫지 않아야 한다. | `echo hi \| nc -N localhost 8080`<br>`ss -ltn 'sport = :8080'` | `LISTEN` 줄이 그대로 있다 |
| S1-31 | close | 서버가 종료되면, 리슨 소켓이 닫혀 8080 포트가 풀려야 한다. | 서버 Ctrl+C 뒤 `ss -ltn 'sport = :8080'` | `LISTEN` 줄이 없다 |
| S1-32 | close | `close()`가 실패하면, 서버는 이유를 출력하고 같은 fd를 다시 닫지 않은 채 다음 `accept()`로 넘어가야 한다. | 코드 리뷰 | `perror("close")`가 있고 재시도나 `exit`가 없다 |

## 알아둘 커널 동작

아래는 커널이 하는 일이라 서버가 지킬 요구사항은 아니지만, 위 요구사항을 이해하는 데 필요해요.

- **3-way handshake**: 소켓이 LISTEN 상태이면, 커널이 클라이언트가 보낸 SYN에 답해서 3-way handshake를 마치고, 맺어진 연결을 accept 큐에 넣는다. 서버가 `accept()`를 부르기 전에도 일어난다.
- **받기 버퍼**: 세그먼트가 도착하면 커널이 해당 소켓의 받기 버퍼에 복사하고 ACK를 보낸다. `read()`는 이 버퍼에서 프로그램 버퍼로 복사하고 지운다.
- **보내기 버퍼**: `write()`는 프로그램 버퍼를 커널 보내기 버퍼로 복사하고 리턴한다. 실제 전송과 재전송은 커널이 하고, ACK를 받은 바이트만 지운다.
- **TIME_WAIT**: 먼저 FIN을 보낸 쪽에만 1분 동안 남는다. Step 1에서는 `nc -N`이 먼저 닫으므로 클라이언트 쪽에 남는다.
