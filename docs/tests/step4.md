# Step 4 테스트

이 문서를 읽으면 Step 4를 어떤 순서로 테스트하고, 결과를 어디에 기록하는지 알 수 있어요. 요구사항은 Step 4 요구사항(S4-01 ~ S4-15), 설계는 [Step 4 설계](../design/step4.md)에 있어요.

## 1. 테스트 범위

| 종류 | 확인하는 것 | 요구사항 |
| --- | --- | --- |
| 단위 테스트 (파싱) | `Content-Length` 읽기, 연결 유지 여부, POST 받기, URI 길이 | S4-03, S4-04, S4-09 ~ S4-11, S4-15 |
| 단위 테스트 (연결) | `handle_conn()`이 본문을 읽고, 요청을 이어 받고, 경계를 모르면 닫는지 | S4-01 ~ S4-12, S4-14, S4-15 |
| 기능 테스트 | 서버를 띄우고 `curl`, `nc`로 요청했을 때, 타임아웃 | S4-01 ~ S4-15 |
| 코드 리뷰 | 메모리 해제, 버퍼 당기기, 타임아웃 설정 | S4-01, S4-08, S4-13 |

### 앞 Step 테스트에서 바뀌는 것

Step 4부터 연결을 유지하고 POST를 받아서, 앞 Step 테스트 몇 개의 기대 결과가 바뀌어요.

| 앞 테스트 | 원래 기대 | 바뀐 기대 | 이유 |
| --- | --- | --- | --- |
| Step 2 UT-23 (`test_http`) | `POST /` → `501` | `0` (파싱 통과) | POST를 받아요 |
| Step 2 UT-40 (`test_server`) | `Connection: close` | `Connection: keep-alive` | HTTP/1.1 기본이 유지예요 |
| Step 2 UT-47 (`test_server`) | 요청 두 개를 붙여 보내면 응답 하나 | 응답 두 개 | 남은 바이트를 다음 요청으로 써요 (S4-08) |
| Step 3 UT-32, UT-33 (`test_server`) | `Connection: close` | `Connection: keep-alive` | 위와 같아요 |

## 2. 준비

- Dev Container 터미널에서 저장소 루트로 이동해요.
- 단위 테스트의 `handle_conn()`은 Step 3처럼 자식 프로세스에서 돌려요. 부모가 요청을 다 쓴 뒤 `shutdown(SHUT_WR)`을 하면, 서버는 응답을 보낸 뒤 다음 요청을 읽다가 `read()`가 `0`이라 끝나요.
- 타임아웃(15초)은 `server_run()`에서 걸어서 단위 테스트에서는 보지 않아요. 기능 테스트 FT-16에서 봐요.

## 3. 단위 테스트: 파싱 (`tests/test_http.c`)

### http_content_length

| 번호 | 헤더 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-01 | `Content-Length` 없음 | `0`, `*out` = `0` | S4-03 |
| UT-02 | `Content-Length: 11` | `1`, `*out` = `11` | S4-01 |
| UT-03 | `Content-Length: 0` | `1`, `*out` = `0` | S4-01 |
| UT-04 | `Content-Length: abc` | `-1` | S4-04 |
| UT-05 | `Content-Length: -5` | `-1` | S4-04 |
| UT-06 | `Content-Length:` (값 없음) | `-1` | S4-04 |
| UT-07 | `Content-Length: 1 1` (가운데 공백) | `-1` | S4-04 |
| UT-08 | `Content-Length: 5` 두 번 | `1`, `*out` = `5` | S4-04 |
| UT-09 | `Content-Length: 5`, `Content-Length: 7` | `-1` | S4-04 |
| UT-10 | `Content-Length: 99999999999999999999999` (넘침) | `-1` | S4-04 |
| UT-11 | `content-length: 3` (소문자 이름) | `1`, `*out` = `3` | S4-01 |

### http_keep_alive

| 번호 | 버전 | `Connection` | 기대 결과 | 요구사항 |
| --- | --- | --- | --- | --- |
| UT-12 | HTTP/1.1 | 없음 | `1` | S4-09 |
| UT-13 | HTTP/1.1 | `close` | `0` | S4-10 |
| UT-14 | HTTP/1.1 | `Close` (대문자) | `0` | S4-10 |
| UT-15 | HTTP/1.1 | `keep-alive` | `1` | S4-09 |
| UT-16 | HTTP/1.0 | 없음 | `0` | S4-11 |
| UT-17 | HTTP/1.0 | `keep-alive` | `1` | S4-11 |
| UT-18 | HTTP/1.0 | `Keep-Alive` (대문자) | `1` | S4-11 |

### 요청 줄 변경

| 번호 | 요청 줄 (뒤에 `Host: x`와 헤더의 끝을 붙임) | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-19 | `POST / HTTP/1.1` | `0` | S4-01 |
| UT-20 | `GET /aaa…` (경로 정확히 4096바이트) | `0` | S4-15 |
| UT-21 | `GET /aaa…` (경로 4097바이트) | `414` | S4-15 |
| UT-22 | `BREW /aaa…` (4097바이트) `HTTP/2.0` | `414` (URI가 메서드·버전보다 먼저) | S4-15 |
| UT-23 | `http_reason()`에 405, 411, 413, 414 | `Method Not Allowed`, `Length Required`, `Content Too Large`, `URI Too Long` | S4-02 ~ S4-15 |

UT-20 ~ UT-22는 요청 줄이 4KB를 넘으니 테스트의 요청 배열을 8KB로 잡아요.

## 4. 단위 테스트: 연결 (`tests/test_server.c`)

부모가 요청을 쓰고 쓰기를 닫은 뒤, 응답을 끝까지 읽어요. 응답 개수는 `HTTP/1.1 `로 시작하는 줄 수로 세요.

### 본문 읽기와 에코

| 번호 | 요청 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-24 | `POST /echo`, `Content-Type: application/x-www-form-urlencoded`, 본문 `name=hyunji` | `200 OK`, 같은 `Content-Type`, `Content-Length: 11`, `Connection: keep-alive`, 본문 `name=hyunji` | S4-01, S4-07 |
| UT-25 | `POST /echo`, `Content-Type` 없음, 본문 `abc` | `Content-Type: application/octet-stream`, 본문 `abc` | S4-07 |
| UT-26 | `POST /echo`, 헤더와 `name=`을 쓰고 0.1초 뒤 `hyunji` | 본문 `name=hyunji` (나뉘어 와도 다 모음) | S4-01 |
| UT-27 | `POST /echo`, `Content-Length: 0` | `200 OK`, `Content-Length: 0`, 본문 없음 | S4-07 |
| UT-28 | `POST /echo`, 본문 0 ~ 255 바이트 256개 (`\0` 포함) | 본문이 바이트 그대로 | S4-07 |

### 남은 바이트와 keep-alive

| 번호 | 요청 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-29 | `GET /style.css` + `GET /index.html` (`Connection: close`)를 한 번에 | 응답 2개가 순서대로, 첫째 `Connection: keep-alive`, 둘째 `Connection: close` | S4-08 ~ S4-10 |
| UT-30 | `POST /echo` 본문 5바이트 + 바로 뒤에 `GET /style.css` | 에코 응답 뒤에 `style.css` 응답 (본문 뒤 바이트를 다음 요청으로 씀) | S4-08 |
| UT-31 | `GET /style.css`, `Content-Length: 5`, 본문 `hello` + 바로 뒤에 `GET /index.html` | 응답 2개 (GET의 본문은 읽고 버림) | S4-08 |
| UT-32 | `GET /nope.html` + `GET /style.css` | `404` 뒤에 `200` (404는 연결을 유지) | S4-08, S4-09 |
| UT-33 | `GET / HTTP/1.0` + `GET /style.css` | 응답 1개, `Connection: close` | S4-11 |
| UT-34 | `GET / HTTP/1.0`, `Connection: keep-alive` + `GET /style.css` | 응답 2개, 첫째 `Connection: keep-alive` | S4-11 |

### 오류

| 번호 | 요청 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-35 | `POST /index.html`, 본문 `a=1` + `GET /style.css` | `405`, `Allow: GET`, 뒤에 `200` (405는 연결을 유지) | S4-02 |
| UT-36 | `GET /echo` | `405`, `Allow: POST` | S4-06 |
| UT-37 | `POST /echo`, `Content-Length` 없음 + `GET /style.css` | `411`, `Connection: close`, 응답 1개 | S4-03, S4-12 |
| UT-38 | `POST /echo`, `Content-Length: abc` | `400`, `Connection: close` | S4-04, S4-12 |
| UT-39 | `POST /echo`, `Content-Length: 5`와 `7` | `400`, `Connection: close` | S4-04, S4-12 |
| UT-40 | `POST /echo`, `Transfer-Encoding: chunked` | `501`, `Connection: close` | S4-05, S4-12 |
| UT-41 | `POST /echo`, `Content-Length: 1048577` (본문은 안 보냄) | `413`, `Connection: close` | S4-14 |
| UT-42 | `POST /echo`, `Content-Length: 1048576`, 본문 1MB | `200`, 1MB가 그대로 | S4-14 |
| UT-43 | `GET /` + 경로 4097바이트 | `414`, `Connection: close` | S4-15 |
| UT-44 | `GET /\r\nHost: x\r\n\r\n` + `GET /style.css` | `400`, `Connection: close`, 응답 1개 | S4-12 |

**통과 기준**: `make test`가 모든 테스트에서 실패 없이 끝나고, `make clean && make test-debug`로 ASan·UBSan을 켜고 돌려도 오류가 없다.

## 5. 기능 테스트

터미널 1에서 `make && make www && ./webserver -p 8080 -r ./www`로 서버를 띄운 상태에서 해요.

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| FT-01 | S4-01 | `(printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\nConnection: close\r\n\r\nname='; sleep 1; printf 'hyunji') \| nc localhost 8080` | 응답 본문이 `name=hyunji` |
| FT-02 | S4-02 | `curl -s -D - -o /dev/null -X POST -d a=1 http://localhost:8080/index.html` | `405 Method Not Allowed`, `Allow: GET` |
| FT-03 | S4-03 | `printf 'POST /echo HTTP/1.1\r\nHost: x\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 411 Length Required` |
| FT-04 | S4-04 | `printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 400 Bad Request` |
| FT-05 | S4-05 | `printf 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 501 Not Implemented` |
| FT-06 | S4-06 | `curl -s -D - -o /dev/null http://localhost:8080/echo` | `405 Method Not Allowed`, `Allow: POST` |
| FT-07 | S4-07 | `curl -s -D - -X POST -d 'name=hyunji' http://localhost:8080/echo` | `200 OK`, `Content-Type: application/x-www-form-urlencoded`, `Content-Length: 11`, 본문 `name=hyunji` |
| FT-08 | S4-08 | `printf 'GET /style.css HTTP/1.1\r\nHost: x\r\n\r\nGET /app.js HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' \| nc localhost 8080 \| grep -c '^HTTP/1.1 200'` | `2` |
| FT-09 | S4-09 | `curl -sv -o /dev/null -o /dev/null http://localhost:8080/index.html http://localhost:8080/style.css 2>&1 \| grep -i 're-using'` | `Re-using existing connection` |
| FT-10 | S4-10 | `printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' \| nc localhost 8080 \| grep -i '^connection'` | `Connection: close`, `nc`가 바로 끝난다 |
| FT-11 | S4-11 | `printf 'GET / HTTP/1.0\r\n\r\n' \| nc localhost 8080 \| grep -i '^connection'` | `Connection: close`, `nc`가 바로 끝난다 |
| FT-12 | S4-11 | `printf 'GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /style.css HTTP/1.0\r\n\r\n' \| nc localhost 8080 \| grep -i '^connection'` | `Connection: keep-alive` 다음 `Connection: close` |
| FT-13 | S4-12 | `printf 'GET /\r\nHost: x\r\n\r\n' \| nc localhost 8080 \| grep -i '^connection'` | `Connection: close`, `nc`가 바로 끝난다 |
| FT-14 | S4-14 | `printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n' \| nc -N localhost 8080 \| head -1` | `HTTP/1.1 413 Content Too Large` |
| FT-15 | S4-15 | `curl -s -o /dev/null -w '%{http_code}\n' "http://localhost:8080/$(head -c 5000 /dev/zero \| tr '\0' a)"` | `414` |
| FT-16 | S4-13 | `time nc localhost 8080 < /dev/null` | 약 15초 뒤 `nc`가 끝난다 (`real 0m15`) |
| FT-17 | S4-13 | FT-16을 돌리는 동안 다른 터미널에서 `curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/` | FT-16이 끝날 때까지 기다렸다가 `200` (Step 5 전까지의 한계) |
| FT-18 | 설계 | 서버를 `make debug && ./webserver -p 8080 -r ./www`로 다시 띄우고 FT-01 ~ FT-15를 보낸 뒤 Ctrl+C | ASan 오류 보고가 없다 |

## 6. 코드 리뷰

`src/http.c`와 `src/server.c`를 보고 확인해요.

| 번호 | 요구사항 | 확인할 것 |
| --- | --- | --- |
| CR-01 | S4-01 | `read_body()`의 `read()`는 `cl - body_len`만큼만 달라고 해서 다음 요청 바이트를 본문 버퍼로 읽지 않는다 |
| CR-02 | 설계 | 본문 버퍼는 응답을 보낸 뒤, 그리고 본문 읽기가 실패한 뒤 모두 `free_body()`로 해제된다 |
| CR-03 | S4-08 | `memmove()`는 `c->len - consumed`바이트를 옮기고, 그 뒤 `c->len`을 `consumed`만큼 줄인다 |
| CR-04 | S4-13 | `accept()` 직후 `SO_RCVTIMEO`를 15초로 건다 |
| CR-05 | S4-13 | `read()`가 `EAGAIN`·`EWOULDBLOCK`이면 오류 출력 없이 닫는다 |
| CR-06 | S4-12 | 400·411·413·414·431·501·505 응답은 `keep`과 관계없이 `Connection: close`이고, 보낸 뒤 연결을 닫는다 |
| CR-07 | S4-14 | `Content-Length`가 1MB를 넘으면 본문 버퍼를 `malloc`하지 않는다 |

## 7. 결과 기록

테스트를 돌린 뒤 이 표를 채워요. 실패하면 비고에 무엇이 나왔는지 적어요.

| 날짜 | 커밋 | 단위 (UT) | 기능 (FT) | 코드 리뷰 (CR) | 비고 |
| --- | --- | --- | --- | --- | --- |
| | | / 44 | / 18 | / 7 | |
