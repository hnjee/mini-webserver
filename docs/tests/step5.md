# Step 5 테스트

이 문서를 읽으면 Step 5를 어떤 순서로 테스트하고, 결과를 어디에 기록하는지 알 수 있어요. 요구사항은 Step 5 요구사항(S5-01 ~ S5-07), 설계는 [Step 5 설계](../design/step5.md)에 있어요.

## 1. 테스트 범위

| 종류 | 확인하는 것 | 요구사항 |
| --- | --- | --- |
| 단위 테스트 (이벤트) | `server_on_accept()`, `conn_on_readable()`, `conn_on_writable()`, `server_sweep_idle()`을 직접 불러 상태가 맞게 바뀌는지 | S5-01 ~ S5-03, S5-05 ~ S5-07 |
| 단위 테스트 (연결) | Step 2 ~ 4의 연결 테스트를 epoll 서버에 그대로 돌려도 통과하는지 | 앞 Step 전체 |
| 기능 테스트 | 서버를 띄우고 `nc`, `curl`, `ab`로 동시에 요청했을 때 | S5-01 ~ S5-07 |
| 코드 리뷰 | 논블로킹, `EAGAIN` 처리, 자원 정리 | S5-01, S5-04 ~ S5-06 |

### 앞 Step 테스트에서 바뀌는 것

`handle_conn()`이 없어져서, `tests/test_server.c`의 연결 테스트를 돌리는 방법이 바뀌어요. 기대 결과는 그대로예요.

| 앞 테스트 | 원래 | 바뀐 것 | 이유 |
| --- | --- | --- | --- |
| Step 2 ~ 4 연결 테스트 (`run_conn`) | 자식 프로세스가 `handle_conn()` 한 번 | 자식 프로세스가 포트 0에 `server_run()`을 띄우고, 부모가 TCP로 접속해 요청을 보냄. 다 읽으면 자식을 끝냄 | 연결 하나만 처리하는 함수가 없어졌어요 |
| `run_conn`의 `handle_conn()` 리턴값 확인 | `0` / `-1` 확인 | 빠짐. 응답 내용과 연결이 닫혔는지만 봐요 | 리턴값을 주는 함수가 없어요 |
| Step 4 FT-17 | FT-16이 끝날 때까지 기다렸다가 `200` | 바로 `200` | Step 5의 목표예요 (S5-01) |
| Step 4 CR-04 | `accept()` 직후 `SO_RCVTIMEO` | 빠짐. CR-02로 대신 | epoll에서는 걸리지 않아요 |

## 2. 준비

- Dev Container 터미널에서 저장소 루트로 이동해요.
- 이벤트 단위 테스트는 한 프로세스 안에서 해요. 포트 0으로 리슨 소켓을 열고 `server_init()`한 뒤, 같은 프로세스에서 클라이언트 소켓으로 `connect()`해요. `epoll_wait()`은 부르지 않고, 알림이 왔다고 치고 함수를 직접 불러요.
- 클라이언트 소켓은 논블로킹으로 만들어서, 응답이 없으면 `read()`가 `EAGAIN`으로 바로 돌아오게 해요. "응답 없음"은 이걸로 확인해요.
- 연결이 닫혔는지는 클라이언트 `read()`가 `0`인지, `s.nconn`이 줄었는지로 봐요.
- 루트는 Step 3처럼 `/tmp` 아래 테스트 폴더를 써요. `big.bin`(1MB, 바이트 패턴)을 더 만들어요.
- 기능 테스트 전에 `make www`로 `large.bin`(5MB)을 만들어요. `www/secret.txt`가 이미 있으면 실패하니 `rm -f www/secret.txt`를 먼저 해요.

## 3. 단위 테스트: 이벤트 (`tests/test_event.c`, 새 파일)

### 연결 받기

| 번호 | 준비 | 동작 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- | --- |
| UT-01 | 접속 없음 | `server_on_accept()` | `0`, `nconn` = 0 (기다리지 않고 돌아옴) | S5-06 |
| UT-02 | 클라이언트 3개 접속 | `server_on_accept()` | `3`, `nconn` = 3, 각 연결 fd에 `O_NONBLOCK`, `state` = `ST_HEADER` | S5-06 |
| UT-03 | 클라이언트 100개 접속 | `server_on_accept()` 세 번 | `64`, `36`, `0` | S5-06 |

### 읽기

| 번호 | 준비 | 동작 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- | --- |
| UT-04 | 연결 1개 | `GET /index.html HTTP/1.1\r\nHo`를 쓰고 `conn_on_readable()` | 돌아옴, `state` = `ST_HEADER`, `len` = 보낸 바이트 수, 클라이언트 `read()`는 `EAGAIN` | S5-02 |
| UT-05 | UT-04 다음 | `st: x\r\n\r\n`을 쓰고 `conn_on_readable()` | `200 OK`, 본문이 `index.html`, `Connection: keep-alive`, `state` = `ST_HEADER`, `len` = 0 | S5-02 |
| UT-06 | 연결 A, B | A에 요청 앞부분, B에 요청 전부를 쓰고 A, B 순서로 `conn_on_readable()` | B만 `200`, A는 `EAGAIN` | S5-01, S5-02 |
| UT-07 | UT-06 다음 | A에 나머지를 쓰고 `conn_on_readable(A)` | A가 `200`, A가 요청한 파일의 본문 | S5-02 |
| UT-08 | 연결 1개 | `POST /echo`, `Content-Length: 11`, 헤더와 `name=`까지 쓰고 `conn_on_readable()` | `state` = `ST_BODY`, 응답 없음 | S5-02 |
| UT-09 | UT-08 다음 | `hyunji`를 쓰고 `conn_on_readable()` | 본문 `name=hyunji`, `state` = `ST_HEADER` | S5-02 |
| UT-24 | 연결 A, B | A에 `POST /echo` 헤더와 본문 `AAAA` 앞부분, B에 `POST /echo` 헤더와 본문 `BBBB` 앞부분을 쓰고 A, B를 번갈아 `conn_on_readable()`, 그다음 각자 나머지 | A는 `AAAA…`만, B는 `BBBB…`만 돌려받음 | S5-07 |
| UT-10 | 연결 1개 | `GET /style.css`와 `GET /index.html`을 한 번에 쓰고 `conn_on_readable()` 한 번 | 응답 2개가 순서대로 (두 번째 요청을 알림 없이 처리) | S5-02 |
| UT-11 | 연결 1개 | `Connection: close` 요청 후 `conn_on_readable()` | 응답 뒤 클라이언트 `read()`가 `0`, `nconn` = 0 | S5-04 |
| UT-12 | 연결 1개 | `BREW / HTTP/1.1\r\nHost: x\r\n\r\n` 후 `conn_on_readable()` | `501`, `Connection: close`, 닫힘 | S5-04 |
| UT-13 | 연결 1개 | 클라이언트가 아무것도 안 보내고 `close()`, `conn_on_readable()` | `nconn` = 0, 열린 fd 수가 접속 전과 같음 | S5-04 |
| UT-14 | 연결 1개 | 요청 앞부분만 보내고 `close()`, `conn_on_readable()` | `nconn` = 0 | S5-04 |

### 보내기

UT-15 ~ UT-18은 서버 쪽 연결 fd의 `SO_SNDBUF`와 클라이언트의 `SO_RCVBUF`를 작게(4KB) 잡아서, 1MB 파일이 한 번에 안 나가게 해요.

| 번호 | 준비 | 동작 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- | --- |
| UT-15 | 연결 1개 | `GET /big.bin` 후 `conn_on_readable()`, 클라이언트는 안 읽음 | 돌아옴(멈추지 않음), `state` = `ST_WRITE`, `want_out` = 1, `0 < file_off < file_size` | S5-05 |
| UT-16 | UT-15 상태 | 연결 B가 `GET /index.html`, `conn_on_readable(B)` | B가 바로 `200` | S5-01, S5-05 |
| UT-17 | UT-15 다음 | 클라이언트가 읽을 수 있는 만큼 읽고 `conn_on_writable()`을 반복 | 받은 본문이 `big.bin`과 바이트 단위로 같음, 끝나면 `want_out` = 0, `state` = `ST_HEADER`, `file_fd` = -1 | S5-05 |
| UT-18 | 연결 1개 | `GET /nope.html` (작은 응답) | 한 번에 다 보냄, `want_out` = 0 | S5-05 |

### 유휴 연결

| 번호 | 준비 | 동작 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- | --- |
| UT-19 | 연결 1개, `last_active`를 과거로 바꿔 둠 | 1바이트 쓰고 `conn_on_readable()` | `last_active`가 지금 시각으로 바뀜 | S5-03 |
| UT-20 | 연결 1개 | `server_sweep_idle(s, last_active + 14)` | `0`, 연결 그대로 | S5-03 |
| UT-21 | 연결 1개 | `server_sweep_idle(s, last_active + 15)` | `1`, 클라이언트 `read()`가 `0` | S5-03 |
| UT-22 | 연결 A, B. A의 `last_active`만 20초 전 | `server_sweep_idle(s, 지금)` | `1`, A만 닫히고 B는 그대로 | S5-03 |

### 정리

| 번호 | 준비 | 동작 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- | --- |
| UT-23 | 연결 5개, 그중 하나는 UT-15처럼 파일을 보내는 중 | `server_free()` | 열린 fd 수가 `server_init()` 전과 같음 | S5-04 |

## 4. 단위 테스트: 연결 (`tests/test_server.c`)

Step 2 ~ 4의 연결 테스트(Step 2 UT-40 ~ 48, Step 3 UT-32 ~ 39, Step 4 UT-24 ~ 44)를 바뀐 `run_conn`으로 그대로 돌려요. 기대 결과는 그 문서 그대로예요. 리턴값 확인만 빠져요.

**통과 기준**: `make test`가 모든 테스트에서 실패 없이 끝나고, `make clean && make test-debug`로 ASan·UBSan을 켜고 돌려도 오류가 없다.

## 5. 기능 테스트

터미널 1에서 `make && make www && ./webserver -p 8080 -r ./www`로 서버를 띄운 상태에서 해요.

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| FT-01 | S5-01 | `nc localhost 8080 &` 다음 `curl -s -o /dev/null -w '%{http_code} %{time_total}\n' http://localhost:8080/index.html` | `200`, 1초 안에 응답 |
| FT-02 | S5-02 | `(printf 'GET /index.html HTTP/1.1\r\nHo'; sleep 2; printf 'st: x\r\nConnection: close\r\n\r\n') \| nc localhost 8080 > a.txt &` 다음 `sleep 0.5; curl -s -o /dev/null -w '%{http_code} %{time_total}\n' http://localhost:8080/index.html`, `wait; head -1 a.txt` | `curl`: `200`, 1초 안에 응답. `a.txt`: `HTTP/1.1 200 OK` |
| FT-03 | S5-03 | `(for i in $(seq 20); do curl -s -o /dev/null http://localhost:8080/; sleep 1; done) &` 다음 `time nc localhost 8080 < /dev/null` | 약 15초 뒤 `nc`가 끝난다 (`real 0m15` ~ `0m16`) |
| FT-04 | S5-04 | `ab -n 2000 -c 50 http://localhost:8080/index.html` | `Failed requests: 0`. `Requests per second`를 결과 기록에 적어요 |
| FT-05 | S5-05 | `curl -s --limit-rate 100K -o /tmp/slow.bin http://localhost:8080/large.bin &` 다음 `sleep 1; curl -s -o /dev/null -w '%{http_code} %{time_total}\n' http://localhost:8080/index.html`, `wait; cmp /tmp/slow.bin www/large.bin && echo same` | 두 번째 `curl`: `200`, 1초 안에 응답. 느린 다운로드(약 50초): `same` |
| FT-06 | S5-06 | `ab -n 2000 -c 200 http://localhost:8080/index.html` | `Failed requests: 0` |
| FT-11 | S5-07 | `{ for i in $(seq 20); do (b="client-$i-$(head -c 3000 /dev/urandom \| base64 -w0)"; r=$(curl -s -X POST --data-binary "$b" http://localhost:8080/echo); [ "$r" = "$b" ] && echo ok \|\| echo FAIL) & done; wait; } \| sort \| uniq -c` | `20 ok` |
| FT-07 | S5-04 | FT-04 뒤에 `ls /proc/$(pgrep -n webserver)/fd \| wc -l` | 서버를 막 띄웠을 때와 같은 수 (연결 fd가 남지 않음) |
| FT-08 | 앞 Step | Step 2 ~ 4 기능 테스트(Step 4 FT-01 ~ FT-16)를 다시 돌림 | 그 문서의 기대 결과 그대로 |
| FT-09 | S5-01 | Step 4 FT-17 (FT-16을 돌리는 동안 다른 `curl`) | 기다리지 않고 바로 `200` |
| FT-10 | 설계 | 서버를 `make debug && ./webserver -p 8080 -r ./www`로 다시 띄우고 FT-01 ~ FT-06을 보낸 뒤 Ctrl+C | ASan 오류 보고가 없다 |

## 6. 코드 리뷰

`src/server.c`를 보고 확인해요.

| 번호 | 요구사항 | 확인할 것 |
| --- | --- | --- |
| CR-01 | S5-01 | 리슨 소켓과 `accept()`로 받은 모든 fd에 `O_NONBLOCK`을 켠다 |
| CR-02 | S5-01 | 이벤트 처리 길에 기다리는 호출이 없다. `write_all()`, 끝날 때까지 읽는 `drain()` 반복, `SO_RCVTIMEO`를 쓰지 않는다 |
| CR-03 | S5-04 | `read()`·`write()`·`accept()`의 `EAGAIN`·`EWOULDBLOCK`은 오류가 아니라 "지금은 없음"으로 처리하고, `epoll_wait()`의 `EINTR`은 다시 돈다 |
| CR-04 | S5-04 | `server_close_conn()`이 `epoll_ctl(DEL)`, `close(fd)`, 파일 fd 닫기, 본문 버퍼 `free`, 리스트에서 빼기, `conn` 해제를 모두 한다 |
| CR-05 | S5-05 | 응답을 다 보내면 `EPOLLOUT`을 끈다 (안 끄면 보내기 버퍼가 빌 때마다 계속 깨어남) |
| CR-06 | S5-05 | `file_off`는 `write()`가 실제로 쓴 바이트만큼만 늘린다 |
| CR-07 | S5-06 | `server_on_accept()`는 최대 `ACCEPT_BATCH`(64)번만 `accept()`한다 |

## 7. 결과 기록

테스트를 돌린 뒤 이 표를 채워요. 실패하면 비고에 무엇이 나왔는지 적어요. FT-04의 처리량은 최종 비교표(DESIGN.md 7장)에도 옮겨요.

| 날짜 | 커밋 | 단위 (UT) | 기능 (FT) | 코드 리뷰 (CR) | 처리량 (FT-04) | 비고 |
| --- | --- | --- | --- | --- | --- | --- |
| | | / 24 | / 11 | / 7 | | |
