# Step 1 테스트

이 문서를 읽으면 Step 1을 어떤 순서로 테스트하고, 결과를 어디에 기록하는지 알 수 있어요. 요구사항은 [Step 1 요구사항](../requirements/step1.md), 설계는 [Step 1 설계](../design/step1.md)에 있어요.

## 1. 테스트 범위

| 종류 | 확인하는 것 | 요구사항 |
| --- | --- | --- |
| 단위 테스트 | `server_listen()`이 리슨 소켓을 맞게 준비하는지, `serve_client()`가 받은 바이트를 그대로 돌려주는지 | S1-01, S1-03, S1-05~S1-07, S1-09, S1-18, S1-20, S1-22, S1-23, S1-25 |
| 기능 테스트 | 서버를 띄우고 `nc`로 접속했을 때 출력, 소켓 상태, fd 수 | S1-01~S1-03, S1-05~S1-10, S1-12~S1-15, S1-18~S1-20, S1-22~S1-25, S1-27~S1-31 |
| 코드 리뷰 | 테스트로 일부러 일으키기 어려운 오류 경로 | S1-04, S1-11, S1-16, S1-17, S1-21, S1-26, S1-32 |

부하 테스트는 Step 5에서 해요. Step 1은 연결을 하나씩만 처리해서 동시 접속 성능을 잴 의미가 없어요.

## 2. 준비

- Dev Container 터미널에서 저장소 루트로 이동해요.
- 기능 테스트는 터미널 3개를 써요. 터미널 1은 서버, 터미널 2와 3은 클라이언트예요.
- `nc`가 `-N` 옵션을 지원하는지 확인해요. `nc -h 2>&1 | grep -- -N`에서 한 줄이 나와야 해요.
- S1-07(1024 미만 포트)은 먼저 아래 값을 확인해요. `0`이면 컨테이너가 일반 사용자에게도 낮은 포트를 허용하는 설정이라, 이 컨테이너에서는 재현할 수 없어요. 그때는 결과에 "해당 없음"과 그 값을 적어요.

```bash
cat /proc/sys/net/ipv4/ip_unprivileged_port_start   # 1024면 재현 가능
```

## 3. 단위 테스트

`tests/test_server.c`에 아래 경우를 모두 넣고 `make test`로 실행해요. 테스트 `main()` 시작에서 `signal(SIGPIPE, SIG_IGN)`을 불러, UT-11에서 테스트 프로그램이 죽지 않게 해요.

### server_listen

포트 `0`을 넘겨 커널이 빈 포트를 고르게 해요. 그래서 8080에 서버가 떠 있어도 부딪히지 않아요.

| 번호 | 하는 일 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-01 | `fd = server_listen(0)` | `fd >= 3` | S1-01 |
| UT-02 | UT-01의 fd로 `getsockopt(SO_ACCEPTCONN)` | 값이 `1` (LISTEN 상태) | S1-09 |
| UT-03 | UT-01의 fd로 `getsockopt(SO_REUSEADDR)` | 값이 `1` | S1-03 |
| UT-04 | UT-01의 fd로 `getsockname()` | 주소가 `INADDR_ANY`(0.0.0.0) | S1-05 |
| UT-05 | `getsockname()`으로 얻은 포트로 `server_listen()`을 한 번 더 호출 | `-1`, 호출 전후 `/proc/self/fd` 개수가 같음 (실패 경로에서 소켓을 닫음) | S1-06 |
| UT-06 | `server_listen(80)` (일반 사용자, 준비 2번의 값이 1024일 때만) | `-1` | S1-07 |

### serve_client

`socketpair(AF_UNIX, SOCK_STREAM, 0, sv)`로 서로 연결된 소켓 두 개를 만들어요. `sv[1]`을 `serve_client()`에 넘기고, 테스트는 `sv[0]` 쪽에서 클라이언트 역할을 해요.

| 번호 | 하는 일 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-07 | `sv[0]`에 `"hi"`를 쓰고 `shutdown(sv[0], SHUT_WR)` 후 `serve_client(sv[1])`, 이어서 `sv[0]`에서 읽기 | 리턴 `0`, 읽은 값 `"hi"` | S1-18, S1-22 |
| UT-08 | 널 문자가 섞인 7바이트 `"a b\nc\0d"`로 UT-07과 같이 | 7바이트가 그대로 돌아옴 | S1-22 |
| UT-09 | 아무것도 쓰지 않고 `shutdown(sv[0], SHUT_WR)` 후 `serve_client(sv[1])` | 리턴 `0`, `sv[0]`에서 읽으면 `0` (돌아온 바이트 없음) | S1-20 |
| UT-10 | `fork()`로 자식 1은 `serve_client(sv[1])`, 자식 2는 `sv[0]`에 1MB를 쓰고 `shutdown`, 부모는 `sv[0]`에서 끝까지 읽기 | 부모가 받은 1MB가 보낸 것과 같고, 자식 1의 종료 코드가 `0` | S1-20, S1-23 |
| UT-11 | `sv[0]`에 `"hi"`를 쓰고 `close(sv[0])` 후 `serve_client(sv[1])` | 리턴 `-1` (`EPIPE`), 테스트 프로그램이 죽지 않음 | S1-25 |
| UT-12 | UT-07이 끝난 뒤 `fcntl(sv[1], F_GETFD)` | `-1`이 아님 (`serve_client()`는 소켓을 닫지 않음) | 설계: `close`는 `server_run()`에서 |

UT-10에서 부모가 쓰기와 읽기를 혼자 하면, 버퍼가 차서 양쪽이 서로 기다리다 멈춰요. 그래서 쓰기는 자식 2에게 맡겨요.

**통과 기준**: `make test`가 `test_config`와 `test_server`에서 실패 없이 끝나고, `make clean && make test-debug`로 ASan·UBSan을 켜고 돌려도 오류가 없다.

## 4. 기능 테스트

터미널 1에서 `make && ./webserver -p 8080 -r ./www`로 서버를 띄운 상태에서 해요. 따로 적은 경우만 서버를 끄거나 다시 띄워요.

### 서버 준비

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| FT-01 | S1-10 | 터미널 1: `./webserver -p 8080 -r ./www` | `listening on 8080`이 나오고 프롬프트로 돌아오지 않는다 |
| FT-02 | S1-01 | `ls -l /proc/$(pgrep -n webserver)/fd \| grep -c socket` | `1` |
| FT-03 | S1-05, S1-09 | `ss -ltn 'sport = :8080'` | `LISTEN` 줄에 `0.0.0.0:8080`이 있다 |
| FT-04 | S1-08 | 서버를 끄고 `strace -e trace=socket,setsockopt,bind,listen ./webserver -p 8080 -r ./www` | 네 시스템 콜이 이 순서로 찍히고 모두 `= 0` 또는 `= 3`으로 성공한다 |
| FT-05 | S1-06 | 서버를 띄운 채 터미널 2: `./webserver -p 8080 -r ./www; echo "exit=$?"` | `bind: Address already in use`, `exit=1` |
| FT-06 | S1-07 | 일반 사용자로 `./webserver -p 80 -r ./www; echo "exit=$?"` | `bind: Permission denied`, `exit=1` (준비의 값이 `0`이면 해당 없음) |
| FT-07 | S1-02 | `strace -o /dev/null -e inject=socket:error=EMFILE ./webserver -p 8081 -r ./www; echo "exit=$?"` | `socket: Too many open files`, `exit=1` |
| FT-08 | S1-03 | 터미널 2: `nc localhost 8080` 유지 → 터미널 1: Ctrl+C → `ss -tan \| grep 8080` → 바로 서버 다시 실행 | `TIME-WAIT` 줄이 보이는데도 `listening on 8080`이 나온다 |

FT-07은 strace로 `socket()`만 `EMFILE`로 실패시켜요. `ulimit -n 3`으로 fd를 막으면 C 라이브러리부터 못 불러와 서버가 시작조차 못 해요. 다른 포트(8081)를 써서 8080 서버와 부딪히지 않게 해요.

### 연결 처리

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| FT-09 | S1-13 | 클라이언트 없이 `ps -o stat,pcpu -p $(pgrep -n webserver)` | `STAT`이 `S`, `%CPU`가 `0.0` |
| FT-10 | S1-12 | 터미널 2: `nc localhost 8080` 유지 → `ls -l /proc/$(pgrep -n webserver)/fd \| grep -c socket` | `2` |
| FT-11 | S1-19 | FT-10 상태 그대로 `ps -o stat,pcpu -p $(pgrep -n webserver)` | `STAT`이 `S`, `%CPU`가 `0.0` |
| FT-12 | S1-27 | FT-10의 `nc`를 Ctrl+C로 끊고 `ls -l /proc/$(pgrep -n webserver)/fd \| grep -c socket` | `1` |
| FT-13 | S1-18 | `echo hi \| nc -N localhost 8080` | `hi` |
| FT-14 | S1-22 | `printf 'a b\nc' \| nc -N localhost 8080 \| od -c` | 보낸 것과 같은 `a b \n c` |
| FT-15 | S1-20, S1-23 | `head -c 1000000 /dev/urandom > /tmp/in`<br>`nc -N localhost 8080 < /tmp/in > /tmp/out`<br>`cmp /tmp/in /tmp/out && echo same` | `same` |
| FT-16 | S1-14 | `echo a \| nc -N localhost 8080`<br>`echo b \| nc -N localhost 8080` | `a`, `b`가 차례로 돌아오고 서버는 계속 떠 있다 |
| FT-17 | S1-15 | 터미널 2: `nc localhost 8080` 유지<br>터미널 3: `echo hi \| nc -N localhost 8080` → 터미널 2를 Ctrl+C | 터미널 3은 터미널 2를 끝낼 때까지 `hi`를 못 받고, 끝내면 바로 받는다 |
| FT-18 | S1-28 | `echo hi \| nc -N localhost 8080; echo "exit=$?"` | `hi` 다음 `exit=0`이 바로 찍힌다 |

### 끊김과 정리

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| FT-19 | S1-24 | `(head -c 50000000 /dev/zero; sleep 30) \| nc localhost 8080 \| sleep 30 &`<br>몇 초 뒤 `ps -o stat,pcpu -p $(pgrep -n webserver)` | `STAT`이 `S`, `%CPU`가 `0.0` |
| FT-20 | S1-25 | FT-19가 끝난 뒤 `head -c 50000000 /dev/zero \| nc localhost 8080 \| head -c 1`<br>`echo hi \| nc -N localhost 8080` | 터미널 1에 `write: Broken pipe`(또는 `Connection reset by peer`)가 찍히고, 서버는 계속 떠 있고 `hi`가 돌아온다 |
| FT-21 | S1-29 | `for i in $(seq 200); do echo hi \| nc -N localhost 8080 > /dev/null; done`<br>`ls -l /proc/$(pgrep -n webserver)/fd \| grep -c socket` | `1` |
| FT-22 | S1-30 | `echo hi \| nc -N localhost 8080`<br>`ss -ltn 'sport = :8080'` | `LISTEN` 줄이 그대로 있다 |
| FT-23 | S1-31 | 터미널 1에서 Ctrl+C 뒤 `ss -ltn 'sport = :8080'` | `LISTEN` 줄이 없다 |

FT-20에서 서버에 찍히는 메시지는 끊긴 시점에 따라 `write: Broken pipe`나 `read: Connection reset by peer` 중 하나예요. 둘 다 그 연결만 닫고 계속 떠 있으면 통과예요.

## 5. 코드 리뷰

`src/server.c`와 `src/main.c`를 보고 확인해요.

| 번호 | 요구사항 | 확인할 것 |
| --- | --- | --- |
| CR-01 | S1-04 | `setsockopt()` 실패 경로에 `perror("setsockopt")`와 `close(fd)`가 있고 `-1`을 돌려준다 |
| CR-02 | S1-11 | `listen()` 실패 경로에 `perror("listen")`과 `close(fd)`가 있고 `-1`을 돌려준다 |
| CR-03 | S1-16 | `accept()`가 `EINTR`이면 출력 없이 다시 부른다 |
| CR-04 | S1-17 | `accept()`가 그 밖의 이유로 실패하면 `perror("accept")` 후 다시 부르고, `EMFILE`·`ENFILE`이면 0.1초 쉰다. `exit`나 `return`이 없다 |
| CR-05 | S1-21 | `read()`가 `EINTR`이면 다시 부른다 |
| CR-06 | S1-26 | `write()`가 `EINTR`이면 남은 부분을 다시 쓴다 |
| CR-07 | S1-32 | `close()`가 실패하면 `perror("close")`만 하고 다시 닫지 않는다 |
| CR-08 | 설계 | `close(client_fd)`는 `server_run()`에 한 번만 있고, `serve_client()`에는 `close`가 없다 |
| CR-09 | 설계 | `server.c`에 `exit()`가 없다. 종료는 `main()`만 한다 |

## 6. 결과 기록

테스트를 돌린 뒤 이 표를 채워요. 실패하면 비고에 무엇이 나왔는지 적어요.

| 날짜 | 커밋 | 단위 (UT) | 기능 (FT) | 코드 리뷰 (CR) | 비고 |
| --- | --- | --- | --- | --- | --- |
| | | / 12 | / 23 | / 9 | |
