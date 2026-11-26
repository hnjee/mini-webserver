# Step 2 테스트

이 문서를 읽으면 Step 2를 어떤 순서로 테스트하고, 결과를 어디에 기록하는지 알 수 있어요. 요구사항은 Step 2 요구사항(S2-01 ~ S2-24), 설계는 [Step 2 설계](../design/step2.md)에 있어요.

## 1. 테스트 범위

| 종류 | 확인하는 것 | 요구사항 |
| --- | --- | --- |
| 단위 테스트 (파싱) | `http.c` 함수가 문자열만으로 요청을 맞게 나누고, 틀린 요청에 맞는 상태 코드를 돌려주는지 | S2-02, S2-03, S2-06, S2-08 ~ S2-14, S2-23, S2-24 |
| 단위 테스트 (연결) | `handle_conn()`이 소켓에서 읽고, 응답을 맞는 모양으로 보내고, 남은 데이터를 비우는지 | S2-02 ~ S2-04, S2-15 ~ S2-20, S2-22 |
| 기능 테스트 | 서버를 띄우고 `nc`, `curl`로 요청했을 때 응답과 연결 상태 | S2-02 ~ S2-05, S2-07 ~ S2-10, S2-13 ~ S2-18, S2-20 ~ S2-24 |
| 코드 리뷰 | 테스트로 보기 어려운 구조와 오류 경로 | S2-01, S2-19, 설계 |

Step 1의 에코 테스트(UT-07 ~ UT-12)는 `serve_client()`가 없어지면서 지워요. `server_listen()` 테스트(UT-01 ~ UT-06)는 Step 1 문서 그대로 남아요.

## 2. 준비

- Dev Container 터미널에서 저장소 루트로 이동해요.
- 기능 테스트는 터미널 2개를 써요. 터미널 1은 서버, 터미널 2는 클라이언트예요.
- 요청은 `printf`로 바이트를 직접 만들어 보내요. `\r\n`, `\0`을 정확히 넣을 수 있어서예요.
- `nc -N`은 보낼 걸 다 보내면 쓰기를 닫아요(FIN). `-N` 없이 쓰면 서버가 먼저 닫아야 `nc`가 끝나요.

## 3. 단위 테스트: 파싱 (`tests/test_http.c`)

요청 문자열을 `char buf[8192]`에 복사해서 넘겨요. `http_parse_request()`가 버퍼 안에 `\0`을 쓰기 때문에 문자열 상수를 바로 넘기면 안 돼요. `len`은 `http_find_header_end()`가 돌려준 값을 써요.

### http_find_header_end

| 번호 | 넣는 것 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-01 | `GET / HTTP/1.1\r\nHost: x\r\n\r\n` (27바이트) | `27` | S2-02 |
| UT-02 | `GET / HTTP/1.1\r\nHost: x\r\n` (헤더의 끝 없음) | `0` | S2-02 |
| UT-03 | `GET / HTTP/1.1\r\nHost: x\r\n\r` (`\n` 하나 모자람) | `0` | S2-02 |
| UT-04 | UT-01 뒤에 `GET /next HTTP/1.1\r\n`이 더 붙음 | `27` (첫 헤더의 끝 위치) | S2-02 |

### http_parse_request: 올바른 요청

| 번호 | 넣는 것 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-05 | `GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n` | `0`, `method`=`GET`, `path`=`/index.html`, `version`=`HTTP/1.1`, `header_count`=`1` | S2-06 |
| UT-06 | `Host:   localhost   ` (값 앞뒤 공백) | 값 `localhost` | S2-11 |
| UT-07 | `Host:\tlocalhost\t` (값 앞뒤 탭) | 값 `localhost` | S2-11 |
| UT-08 | `Host: a.com:8080` (값 안에 `:`) | 이름 `Host`, 값 `a.com:8080` (첫 `:`에서만 나눔) | S2-11 |
| UT-09 | `host: x`로 받고 `http_find_header(req, "Host")` | `x` | S2-12 |
| UT-10 | `Host: x`로 받고 `http_find_header(req, "HOST")` | `x` | S2-12 |
| UT-11 | UT-05로 받고 `http_find_header(req, "Cookie")` | `NULL` | S2-12 |
| UT-12 | `GET / HTTP/1.0\r\n\r\n` (HTTP/1.0, `Host` 없음) | `0`, `header_count`=`0` | S2-14 |
| UT-13 | `Host` + `X-1` ~ `X-31` (헤더 32줄) | `0`, `header_count`=`32` | S2-03 |

### http_parse_request: 요청 줄 오류

| 번호 | 요청 줄 (뒤에 `Host: x`와 헤더의 끝을 붙임) | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-14 | `GET /` (두 부분) | `400` | S2-08 |
| UT-15 | `GET / HTTP/1.1 extra` (네 부분) | `400` | S2-08 |
| UT-16 | `GET  / HTTP/1.1` (공백 두 개) | `400` | S2-08 |
| UT-17 | ` / HTTP/1.1` (메서드 비었음) | `400` | S2-08 |
| UT-18 | `GET index.html HTTP/1.1` (경로가 `/`로 시작 안 함) | `400` | S2-08 |
| UT-19 | `GET / HTTP/1` | `400` | S2-08 |
| UT-20 | `GET / HTTPS/1.1` | `400` | S2-08 |
| UT-21 | `GET / HTTP/a.b` | `400` | S2-08 |
| UT-22 | `BREW / HTTP/1.1` | `501` | S2-09 |
| UT-23 | `POST / HTTP/1.1` | `501` | S2-09 |
| UT-24 | `get / HTTP/1.1` (소문자) | `501` | S2-09 |
| UT-25 | `GET / HTTP/2.0` | `505` | S2-10 |
| UT-26 | `GET / HTTP/0.9` | `505` | S2-10 |

### http_parse_request: 헤더 오류

| 번호 | 헤더 줄 (요청 줄은 `GET / HTTP/1.1`) | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-27 | `Host x` (`:` 없음) | `400` | S2-13 |
| UT-28 | `: x` (이름 비었음) | `400` | S2-13 |
| UT-29 | `Host : x` (`:` 앞 공백) | `400` | S2-13 |
| UT-30 | `Host\t: x` (`:` 앞 탭) | `400` | S2-13 |
| UT-31 | 헤더 없음 (`GET / HTTP/1.1\r\n\r\n`) | `400` (HTTP/1.1인데 `Host` 없음) | S2-14 |
| UT-32 | `Host` + `X-1` ~ `X-32` (헤더 33줄) | `431` | S2-03 |
| UT-33 | `Host: x\0y` (헤더 값 안에 `\0`) | `400` | S2-24 |
| UT-34 | `GET /a\0b HTTP/1.1` (요청 줄 안에 `\0`) | `400` | S2-24 |

### http_parse_request: 검사 순서

| 번호 | 넣는 것 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-35 | `BREW / HTTP/2.0` + `Host: x` (메서드, 버전 둘 다 틀림) | `501` (메서드가 먼저) | S2-23 |
| UT-36 | `BREW index HTTP/2.0` + `Host: x` (형식도 틀림) | `400` (형식이 먼저) | S2-23 |
| UT-37 | `GET / HTTP/2.0` + `Host x` (버전, 헤더 둘 다 틀림) | `505` (버전이 헤더보다 먼저) | S2-23 |
| UT-38 | `BREW / HTTP/1.1` + 헤더 33줄 | `431` (줄 수가 파싱보다 먼저) | S2-03, S2-23 |

### http_reason

| 번호 | 넣는 것 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-39 | `200`, `400`, `431`, `501`, `505` | `OK`, `Bad Request`, `Request Header Fields Too Large`, `Not Implemented`, `HTTP Version Not Supported` | S2-20 |

## 4. 단위 테스트: 연결 (`tests/test_server.c`)

`socketpair(AF_UNIX, SOCK_STREAM, 0, sv)`로 연결된 소켓 두 개를 만들어요. `conn`을 `malloc`해서 `fd`=`sv[1]`, `len`=`0`으로 채우고 `handle_conn()`에 넘겨요. 테스트는 `sv[0]` 쪽에서 클라이언트 역할을 해요. 요청을 다 쓴 뒤 `shutdown(sv[0], SHUT_WR)`을 해야 `drain()`이 끝나요.

| 번호 | 하는 일 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-40 | `GET / HTTP/1.1\r\nHost: x\r\n\r\n`을 쓰고 `handle_conn()`, `sv[0]`에서 끝까지 읽기 | 리턴 `0`, 읽은 값이 정확히 `HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n` | S2-15 ~ S2-17 |
| UT-41 | `fork()`로 자식이 `GET / HTTP/1.1\r\n`을 쓰고 0.1초 뒤 `Host: x\r\n\r\n`을 씀 | 응답 상태 줄이 `HTTP/1.1 200 OK` | S2-02 |
| UT-42 | 헤더의 끝까지 정확히 8192바이트인 요청 (`X:` 값으로 길이 맞춤) | `HTTP/1.1 200 OK` | S2-03 |
| UT-43 | 8193바이트인 요청 | `HTTP/1.1 431 Request Header Fields Too Large` | S2-03 |
| UT-44 | `GET / HTTP/1.1\r\n`만 쓰고 `shutdown` | 리턴 `-1`, `sv[0]`에서 읽으면 `0` (아무 응답 없음) | S2-04 |
| UT-45 | `BREW / HTTP/1.1\r\nHost: x\r\n\r\n` | 상태 줄 `HTTP/1.1 501 Not Implemented`, `Content-Type: text/plain`, `Content-Length` 값이 본문 바이트 수와 같음, 본문 `501 Not Implemented\n` | S2-09, S2-20 |
| UT-46 | `BREW / HTTP/1.0\r\n\r\n` | 상태 줄이 `HTTP/1.1 501 Not Implemented` (요청이 1.0이어도 1.1) | S2-22 |
| UT-47 | 올바른 요청 바로 뒤에 `GET /next HTTP/1.1\r\nHost: x\r\n\r\n`을 이어 씀 | 응답이 한 개뿐 (`HTTP/1.1`로 시작하는 줄이 하나) | S2-02 |
| UT-48 | UT-43 뒤 `recv(sv[1], …, MSG_DONTWAIT)` | `0` (남은 데이터를 `drain()`이 다 비움) | 설계: drain |

UT-41과 UT-42, UT-43에서 큰 요청을 쓰면서 응답을 기다리면 양쪽이 서로 기다릴 수 있어요. 그래서 쓰기는 자식에게 맡기고 부모는 `handle_conn()`을 돌린 뒤 읽어요.

**통과 기준**: `make test`가 `test_config`, `test_server`, `test_http`에서 실패 없이 끝나고, `make clean && make test-debug`로 ASan·UBSan을 켜고 돌려도 오류가 없다.

## 5. 기능 테스트

터미널 1에서 `make && ./webserver -p 8080 -r ./www`로 서버를 띄운 상태에서 해요.

### 정상 응답

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| FT-01 | S2-07 | `curl -sv http://localhost:8080/ 2>&1 \| grep '< HTTP'` | `< HTTP/1.1 200 OK` |
| FT-02 | S2-16 | `curl -sv http://localhost:8080/ 2>&1 \| grep '^<'` | `Content-Length: 0`, `Connection: close`가 보인다 |
| FT-03 | S2-15, S2-17 | `printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' \| nc -N localhost 8080 \| od -c` | `HTTP/1.1 200 OK`로 시작하고, 줄마다 `\r \n`, `\r \n \r \n`에서 끝나고 그 뒤가 없다 |
| FT-04 | S2-02 | `(printf 'GET / HTTP/1.1\r\n'; sleep 1; printf 'Host: x\r\n\r\n') \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 200 OK` |
| FT-05 | S2-14 | `printf 'GET / HTTP/1.0\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 200 OK` |
| FT-06 | S2-18 | `printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' \| nc localhost 8080; echo "exit=$?"` | `-N` 없이도 응답 뒤 `exit=0`이 바로 나온다 |

### 오류 응답

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| FT-07 | S2-08 | `printf 'GET /\r\nHost: x\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 400 Bad Request` |
| FT-08 | S2-09 | `printf 'BREW / HTTP/1.1\r\nHost: x\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 501 Not Implemented` |
| FT-09 | S2-09 | `curl -s -o /dev/null -w '%{http_code}\n' -X POST -d a=1 http://localhost:8080/` | `501` |
| FT-10 | S2-10 | `printf 'GET / HTTP/2.0\r\nHost: x\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 505 HTTP Version Not Supported` |
| FT-11 | S2-13 | `printf 'GET / HTTP/1.1\r\nHost x\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 400 Bad Request` |
| FT-12 | S2-14 | `printf 'GET / HTTP/1.1\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 400 Bad Request` |
| FT-13 | S2-24 | `printf 'GET / HTTP/1.1\r\nHost: x\0y\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 400 Bad Request` |
| FT-14 | S2-20 | `curl -s -X BREW -D - -o /tmp/body http://localhost:8080/ \| grep -i content-length`<br>`wc -c < /tmp/body` | `Content-Length` 값과 `wc -c` 숫자가 같고 `0`보다 크다 |
| FT-15 | S2-21 | `printf 'GET /\r\n\r\n' \| nc localhost 8080; echo "exit=$?"`<br>`curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/` | `400` 응답 뒤 `exit=0`, 이어서 `200` |
| FT-16 | S2-22 | `printf 'BREW / HTTP/1.0\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 501 Not Implemented` |
| FT-17 | S2-23 | `printf 'BREW / HTTP/2.0\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 501 Not Implemented` |

### 크기 제한과 끊김

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| FT-18 | S2-03 | `{ printf 'GET / HTTP/1.1\r\nX: '; head -c 9000 /dev/zero \| tr '\0' a; printf '\r\n\r\n'; } \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 431 Request Header Fields Too Large` (RST 없이 받음) |
| FT-19 | S2-03 | `{ printf 'GET / HTTP/1.1\r\nHost: x\r\n'; for i in $(seq 31); do printf "X-$i: a\r\n"; done; printf '\r\n'; } \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 200 OK` (헤더 32줄) |
| FT-20 | S2-03 | FT-19에서 `seq 31`을 `seq 32`로 | `HTTP/1.1 431 Request Header Fields Too Large` (헤더 33줄) |
| FT-21 | S2-04 | `printf 'GET / HTTP/1.1\r\n' \| nc -N localhost 8080; echo "exit=$?"` | 아무 응답 없이 `exit=0`, 서버는 계속 떠 있다 |
| FT-22 | S2-05 | 서버를 `make debug && ./webserver -p 8080 -r ./www`로 다시 띄우고 FT-01 ~ FT-21을 보낸 뒤 Ctrl+C | ASan(LeakSanitizer) 누수 보고가 없다 |

FT-18은 서버가 8192바이트까지만 읽고 431을 보낸 뒤, `drain()`이 남은 808바이트를 비우고 닫아요. RST가 나가면 `nc`가 `Connection reset by peer`를 찍거나 아무것도 못 받아요. 그러면 실패예요.

## 6. 코드 리뷰

`src/http.c`와 `src/server.c`를 보고 확인해요.

| 번호 | 요구사항 | 확인할 것 |
| --- | --- | --- |
| CR-01 | S2-01 | `struct conn` 안에 크기가 고정된 8KB 헤더 버퍼가 있고, 본문 버퍼는 없다 |
| CR-02 | S2-19 | 응답 전송이 `write_all()`을 쓰고, `write_all()`은 일부만 쓰이면 남은 바이트를 이어서 쓴다 |
| CR-03 | 설계 | `handle_conn()`의 `read()`가 `EINTR`이면 다시 부른다 |
| CR-04 | S2-05 | `close()`와 `free()`는 `server_run()`에 한 번씩만 있다 |
| CR-05 | 설계 | `malloc()`이 실패하면 `perror("malloc")`, `close(client_fd)` 후 다음 `accept()`로 간다 |
| CR-06 | 설계 | `drain()`은 `shutdown(fd, SHUT_WR)` 뒤에 읽고, 16KB(`DRAIN_LIMIT`)를 넘게 읽지 않는다 |
| CR-07 | 설계 | `http.c`에는 `read`, `write`, `socket` 같은 시스템 콜이 없다. 버퍼만 다룬다 |
| CR-08 | 설계 | `http.c`와 `server.c`에 `exit()`가 없다 |

## 7. 결과 기록

테스트를 돌린 뒤 이 표를 채워요. 실패하면 비고에 무엇이 나왔는지 적어요.

| 날짜 | 커밋 | 단위 (UT) | 기능 (FT) | 코드 리뷰 (CR) | 비고 |
| --- | --- | --- | --- | --- | --- |
| | | / 48 | / 22 | / 8 | |
