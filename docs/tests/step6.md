# Step 6 테스트

이 문서를 읽으면 Step 6(워커 스레드, 리버스 프록시)을 어떤 순서로 테스트하고, 결과를 어디에 기록하는지 알 수 있어요. 요구사항은 Step 6 요구사항(S6-01 ~ S6-10), 설계는 [Step 6 설계](../design/step6.md)에 있어요.

## 1. 테스트 범위

| 종류 | 파일 | 확인하는 것 | 요구사항 |
| --- | --- | --- | --- |
| 단위 (설정) | `tests/test_config.c` | `-t` 인자 | S6-01 |
| 단위 (라우팅 표) | `tests/test_route.c` | `routes.conf` 읽기, 경로 맞추기, 라운드 로빈, 스레드 4개가 같이 써도 정확히 번갈아 | S6-02, S6-05 |
| 단위 (프록시) | `tests/test_proxy.c` | 요청 넘기기, 응답 중계, 502·504, 끊김 정리 | S6-02 ~ S6-10 |
| 단위 (스레드) | `tests/test_server.c` | 워커 4개면 스레드 5개, Step 2 ~ 5 연결 테스트를 워커 2개로 다시 | S6-01 |
| 기능 | 서버 + `python3 -m http.server` 백엔드 둘 | 시트의 S6-01 ~ S6-10 확인 명령어 | S6-01 ~ S6-10 |

### 앞 Step 테스트에서 바뀌는 것

| 앞 테스트 | 바뀐 것 | 이유 |
| --- | --- | --- |
| `test_server.c`의 `run_conn` | `server_run(lfd, root, NULL, 2)`로 워커 2개를 띄움 | `server_run()` 인자가 늘었어요. 워커가 여럿이어도 Step 2 ~ 5 기대 결과가 그대로인지 같이 봐요 |
| `test_event.c` | 그대로 | 이벤트 함수 이름과 동작은 그대로예요. 리슨 소켓 등록에 `EPOLLEXCLUSIVE`만 붙었어요 |

## 2. 준비

- `test_route.c`는 `/tmp`에 `routes.conf`를 만들어 써요.
- `test_proxy.c`는 한 프로세스 안에서 서버, 클라이언트, 가짜 백엔드를 모두 만들어요. 가짜 백엔드는 테스트가 연 리슨 소켓이고, `accept()`, 요청 읽기, 응답 쓰기, 닫기를 테스트가 직접 해요.
- 서버 알림은 `server_poll_once(s, 20)`로 처리해요.
- 시간 제한(3초, 10초)은 기다리지 않고 `server_sweep_idle(s, 지금 + N)`으로 시각을 건너뛰어 확인해요.

## 3. 단위 테스트: 설정과 라우팅 표

### `test_config.c` (S6)

| 번호 | 입력 | 기대 결과 |
| --- | --- | --- |
| S6 UT-09 | `-t 4` | OK, `threads == 4` |
| S6 UT-10 | `-t` 없음 | OK, `threads == 0` (main이 코어 수 × 2로 정함) |
| S6 UT-11 | `-t 1`, `-t 256` | OK (경곗값) |
| S6 UT-12 | `-t 0`, `-t 257`, `-t abc` | `CONFIG_ERR_THREADS` |
| S6 UT-13 | `-t 2 -t 3` | `CONFIG_ERR_USAGE` |

### `test_route.c`

| 번호 | 입력 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-01 | `/api/ 127.0.0.1:8001 127.0.0.1:8002` | 규칙 1개, 백엔드 2개, 포트 8001·8002 | S6-02 |
| UT-02 | 주석, 빈 줄, 탭 | 건너뛰고 규칙 1개 | S6-02 |
| UT-03 | `/api/`와 `/api/v2/` | `/api/v2/x` → `/api/v2/`, `/api/x` → `/api/` (긴 경로 먼저) | S6-02 |
| UT-04 | `/index.html`, `/api` | 맞는 규칙 없음 | S6-02 |
| UT-05 | `/api/x?y=1` | `/api/` | S6-02 |
| UT-06 | 백엔드 없음, 포트 `abc`·0·70000, `localhost:8001`, 콜론 없음, `/` 없는 경로, 규칙 17개, 백엔드 9개, 없는 파일 | 모두 -1 | S6-02 |
| UT-07 | 4번 고르기 | 8001, 8002, 8001, 8002 | S6-05 |
| UT-08 | 스레드 4개가 1만 번씩 | 정확히 2만, 2만 | S6-05 |

## 4. 단위 테스트: 프록시 (`test_proxy.c`)

| 번호 | 준비와 동작 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-14 | `GET /api/x?y=1`, `Connection: keep-alive`, `X-Test: 1` | 백엔드가 받은 요청 줄은 쿼리까지 그대로, `Host`와 `X-Test` 그대로, `Connection: close`로 바뀜 | S6-02, S6-03 |
| UT-15 | 백엔드가 `HTTP/1.0 200 OK … 8001`을 보내고 닫음 | 클라이언트가 바이트 그대로 받음 | S6-04 |
| UT-16 | UT-15 다음 | 클라이언트 연결이 닫힘, `nconn == 0` | S6-04 |
| UT-17 | `POST /api/echo`, 본문 `abc` | 백엔드가 `Content-Length: 3`과 본문 `abc`를 받음 | S6-03 |
| UT-18 | 백엔드 A, B에 요청 4번 | A, B, A, B | S6-05 |
| UT-19 | `GET /index.html` | 200, keep-alive, 백엔드로 안 감 | S6-09 |
| UT-20 | 백엔드가 1MB, 클라이언트 받기 버퍼 4KB | 1MB가 바이트 그대로 | S6-04 |
| UT-21 | UT-20 동안 | 중계 버퍼가 64KB를 넘지 않음, 끝나면 닫힘 | S6-04 |
| UT-22 | 아무도 안 듣는 포트 | 502, `Connection: close`, 닫힘 | S6-06 |
| UT-23 | accept 큐가 가득 찬 백엔드 (연결이 안 맺어짐), `server_sweep_idle(지금 + 4)` | 504, 닫힘 (이 커널에서 흉내가 안 되면 skip) | S6-07 |
| UT-24 | 백엔드가 아무것도 안 보냄, `지금 + 8`, `지금 + 11` | 8초에는 그대로 (초가 넘어가도 흔들리지 않게 2초 여유), 11초에 504, 백엔드 연결도 닫힘 | S6-07 |
| UT-25 | 백엔드가 일부만 보내고 멈춤, `지금 + 11` | 받은 데까지 넘기고 504 없이 닫힘 | S6-07 |
| UT-26 | 백엔드가 한 바이트도 없이 닫음 | 502, 닫힘 | S6-10 |
| UT-27 | 중계 중에 클라이언트가 끊음 | `nconn == 0`, 백엔드 연결도 닫힘, 열린 fd 수가 처음과 같음 | S6-10 |
| UT-28 | 백엔드가 응답 안 하는 동안 다른 클라이언트가 `GET /index.html` | 200 | S6-08 |

## 5. 단위 테스트: 스레드 (`test_server.c`)

| 번호 | 동작 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| S6 UT-29 | 자식 프로세스에서 `server_run(lfd, root, NULL, 4)` | `/proc/<pid>/task` 항목 5개 (메인 1 + 워커 4) | S6-01 |
| (앞 Step) | Step 2 ~ 5 연결 테스트를 워커 2개로 | 기대 결과 그대로 | S6-01 |

**통과 기준**: `make test`가 모두 통과하고, `make clean && make test-debug`(ASan·UBSan)도 오류가 없다.

## 6. 기능 테스트

백엔드 준비:

```
mkdir -p /tmp/b1/api /tmp/b2/api
echo 8001 > /tmp/b1/api/whoami.txt; echo 8002 > /tmp/b2/api/whoami.txt
head -c 5000000 /dev/urandom > /tmp/b1/api/large.bin; cp /tmp/b1/api/large.bin /tmp/b2/api/
(cd /tmp/b1 && python3 -m http.server 8001 &); (cd /tmp/b2 && python3 -m http.server 8002 &)
printf '/api/ 127.0.0.1:8001 127.0.0.1:8002\n' > routes.conf
./webserver -p 8080 -r ./www -c routes.conf -t 4
```

시트 Step 6 탭의 확인 명령어(S6-01 ~ S6-10)를 차례로 돌려요. `backend1/`은 `/tmp/b1/`로 바꿔 읽어요.

| 번호 | 요구사항 | 기대 결과 |
| --- | --- | --- |
| FT-01 | S6-01 | 스레드 5개, `ab -n 2000 -c 200` 실패 0, Step 5 기능 테스트 통과 |
| FT-02 | S6-02 | `/api/whoami.txt`는 백엔드 파일, `/index.html`은 www 파일 |
| FT-03 | S6-03 | `nc`가 받은 요청에 요청 줄, `Host`, `Connection: close`, 본문 |
| FT-04 | S6-04 | `large.bin`이 원본과 같음, `Closing connection` |
| FT-05 | S6-05 | 8001, 8002, 8001, 8002 |
| FT-06 | S6-06 | 둘 다 끄면 502 |
| FT-07 | S6-07 | `nc -l 8001`에 약 10초 뒤 504 |
| FT-08 | S6-08 | 백엔드가 응답 안 하는 동안 다른 `curl` 200, 1초 안 |
| FT-09 | S6-09 | Step 3 기능 테스트 통과 |
| FT-10 | S6-10 | 받다 끊어도 서버가 살아 있고 fd 수가 돌아옴, `nc -N -l 8001 < /dev/null`이면 502 |

## 7. 결과 기록

| 날짜 | 커밋 | 단위 | 기능 (FT) | 비고 |
| --- | --- | --- | --- | --- |
| 2026-11-25 | (구현 커밋) | 모두 통과 | / 10 | 클라우드 리눅스에서 `test_config` 30/30, `test_route` 23/23, `test_proxy` 28/28(5번 반복), `test_server` 60/60, `test_event` 45/45, `make test-debug`도 통과. 같은 곳에서 FT-01(스레드 5개), FT-02, FT-04, FT-05, FT-06, FT-07(9.5초), FT-08, FT-10(`nc -N`) 확인. Dev Container 기록은 아직 |
