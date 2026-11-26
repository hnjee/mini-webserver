# Step 7 테스트

이 문서를 읽으면 Step 7(안정성)과 Step 8(측정)을 어떻게 확인했고 결과가 어땠는지 알 수 있어요. 설계는 [Step 7 설계](../design/step7.md)에 있어요.

## 1. 단위 테스트 (`tests/test_stable.c`)

UT-01 ~ UT-16과 UT-21은 서버 함수를 직접 부르고, 시간은 `server_sweep_idle(s, 지금 + N)`으로 건너뛰어요. UT-17 ~ UT-20은 자식 프로세스에서 `server_run()`(워커 2개)을 돌리고 진짜 신호를 보내요.

| 번호 | 동작 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-01 | 쉬는 연결, 헤더 일부 보낸 연결, 1MB 받는 연결에서 `server_begin_stop()` | 쉬는 연결 하나만 응답 없이 닫힘 | S7-02 |
| UT-02 | 그 뒤 새로 접속 | 받지 않음 | S7-02 |
| UT-03 | 헤더 마저 보냄 | 200, `Connection: close`, 닫힘 | S7-02 |
| UT-04 | 1MB 받는 연결 계속 읽음 | 1MB 그대로, 다 보낸 뒤 닫힘 | S7-02 |
| UT-05 | `server_should_exit()` | 종료 전 0, 연결 있고 4초 0, 5초 1, 연결 0개면 바로 1 | S7-02 |
| UT-06 | `eventfd`에 1 쓰기 | 종료 시작, 그 뒤 다시 안 깸 | S7-01 |
| UT-07 | 헤더 일부, 29초 | 그대로 | S7-04 |
| UT-08 | 30초 넘김 | `408 Request Timeout`, 닫힘 | S7-04 |
| UT-09 | 아무것도 안 보낸 연결, 31초 | 408 없이 닫힘 (유휴 15초) | S7-04 |
| UT-10 | `GET /index.html` | 로그 한 줄: 날짜 시각, `127.0.0.1`, 요청 줄, 200, 받은 바이트 수, `ms` | S7-06 |
| UT-11 | keep-alive로 두 요청 (200, 404) | 두 줄, 404도 남음 | S7-06 |
| UT-12 | 408 | 요청 줄 `"-"` | S7-06 |
| UT-13 | 1MB 받다 끊음 | 한 줄, 보낸 바이트 < 1MB | S7-06 |
| UT-14 | 로그 fd -1 | 아무것도 안 씀 | S7-06 |
| UT-15 | fd 한도를 256으로 낮추고 다 씀, 손님 대기 | 쉬기 시작, 0.2초 동안 안 깸 (헛돌지 않음) | S7-05 |
| UT-16 | fd 풀고 1초 뒤 | 다시 감시, 기다리던 손님 받음 | S7-05 |
| UT-17 | 쉬는 keep-alive 연결 있을 때 `SIGTERM` | 1초 안에 종료, 종료 코드 0 | S7-01, S7-02 |
| UT-18 | 헤더 일부만 보낸 클라이언트 있을 때 `SIGINT` | 약 5초 뒤 종료, 코드 0 | S7-02 |
| UT-19 | `SIGINT` 두 번 | 1초 안에 종료, 코드 130 | S7-02 |
| UT-20 | 1MB 받는 중 `SIGTERM` | 1MB 끝까지 받고 닫힘, 종료 코드 0 | S7-02 |
| UT-21 | 받은 소켓 | `TCP_NODELAY` 켜짐 | (성능) |
| - | 끝 | 열린 fd 수가 처음과 같음 | S7-03 |

## 2. 기능 테스트

준비는 [Step 6 테스트](step6.md) 6장과 같아요. 웹 루트는 리눅스 폴더(`/tmp/www`)를 써요. 윈도우 폴더 연결은 파일 읽기가 느려서 처리량이 낮게 나와요.

| 번호 | 명령 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| FT-01 | `curl --limit-rate 200k -m 1 localhost:8080/large.bin -o /dev/null` 뒤 `curl localhost:8080/index.html` | 서버가 살아 있고 200 | S7-01 |
| FT-02 | `make debug` 서버에 요청 몇 개, `ab`, 끊긴 다운로드 뒤 `Ctrl+C` | ASan 오류·누수 보고 없음, "종료했어요" | S7-03 |
| FT-03 | 일반 빌드를 `valgrind --leak-check=full`로 띄우고 같은 일 뒤 `kill -INT` | `All heap blocks were freed`, `ERROR SUMMARY: 0` | S7-03 |
| FT-04 | `curl --limit-rate 2M localhost:8080/large.bin -o got.bin &` 다음 0.5초 뒤 `Ctrl+C` | `cmp got.bin www/large.bin` 같음, 그 뒤 서버 종료 | S7-02 |
| FT-05 | `(printf 'GET / HTTP/1.1\r\n'; sleep 20) \| nc localhost 8080 &` 다음 `Ctrl+C` | 약 5초 뒤 종료 | S7-02 |
| FT-06 | FT-07처럼 바이트를 흘리는 클라이언트를 붙여 두고 `Ctrl+C` 두 번 | "바로 꺼요", 바로 종료 | S7-02 |
| FT-07 | `(printf 'GET / HTTP/1.1\r\n'; for i in 1 2 3 4; do sleep 10; printf 'X'; done) \| nc localhost 8080` (10초마다 1바이트) | 약 30초 뒤 `408 Request Timeout`, 로그에 `"-" 408` | S7-04 |
| FT-08 | `wrk -t4 -c100 -d10s` 중 화면 로그 | 모든 줄이 형식대로, 섞인 줄 없음 | S7-06 |

## 3. 부하 측정 (Step 8)

`make` 일반 빌드, 워커 기본값(코어 × 2), 루트 `/tmp/www`, 백엔드는 `python3 -m http.server` 두 개예요.

| 도구와 명령 | Requests/sec | 실패 |
| --- | --- | --- |
| `ab -n 20000 -c 200 /index.html` | 약 28,000 ~ 29,500 | 0 |
| `ab -n 20000 -c 50 /index.html` | 약 25,000 | 0 |
| `wrk -t4 -c100 -d10s /index.html` | 약 109,000 (지연 평균 1.3ms) | 0 |
| `ab -n 2000 -c 50 /api/whoami.txt` (프록시) | 약 1,080 | 0 |
| `wrk -t4 -c20 -d10s /api/whoami.txt` (프록시) | 약 2,600 | 시간 초과 3 |

- `ab`는 HTTP/1.0이라 요청마다 새 연결을 맺어서 keep-alive를 쓰는 `wrk`보다 낮게 나와요.
- 프록시는 백엔드(`python3 -m http.server`, 스레드 하나)가 병목이에요. 시간 초과 3건도 백엔드 쪽이에요.
- `TCP_NODELAY`를 켜기 전 `wrk` 정적 파일은 약 2,270, 지연 평균 44ms였어요(설계 7장).

## 4. 결과 기록

| 날짜 | 커밋 | 단위 | 기능 (FT) | 비고 |
| --- | --- | --- | --- | --- |
| 2026-11-26 | (구현 커밋) | `test_stable` 46/46, 전체 통과, `make test-debug` 통과 | 8/8 | 클라우드 리눅스(코어 2). 노트북 Dev Container 측정은 아직. 헤더 일부만 보내고 조용한 클라이언트는 408 전에 유휴 15초로 먼저 닫혀요(응답 없음) |
