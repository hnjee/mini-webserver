# mini-webserver

C와 POSIX 소켓만으로 만든 **HTTP 웹 서버**예요. 정적 파일을 보내고, `/api/` 같은 경로는 뒤에 있는 백엔드 서버들에 번갈아 넘겨 주는 **리버스 프록시**도 해요. 워커 스레드마다 epoll을 하나씩 돌려서, 연결 수천 개를 스레드 몇 개로 동시에 처리해요.

| 구분 | 사용 기술 |
| --- | --- |
| 언어 | C11 (표준 C와 POSIX API만 사용) |
| 동시성 | 워커 스레드 N개 × epoll (기본 CPU 코어 수 × 2) |
| 통신 | HTTP/1.0, HTTP/1.1 (keep-alive) |
| 프록시 | 경로 규칙(`routes.conf`), 라운드 로빈, 502·504 |
| 검사 | 단위 테스트 9개 파일, ASan·UBSan, valgrind |

이 문서를 읽으면 서버를 빌드해서 켜고, `curl`로 정적 파일과 프록시 요청을 보내 볼 수 있어요. 처음이면 1번부터, 시연만 보려면 [6. 1분 시연](#6-1분-시연)으로 가요.

## 1. 빌드

리눅스에서 빌드해요. 이 저장소의 Dev Container(`.devcontainer/`)를 열면 필요한 도구가 다 들어 있어요.

```bash
make            # ./webserver 생성
make www        # 테스트용 큰 파일(large.bin)과 읽기 금지 파일(secret.txt) 만들기
```

| 명령 | 하는 일 |
| --- | --- |
| `make` | 일반 빌드 (`-Wall -Wextra -Werror`) |
| `make debug` | ASan·UBSan을 켠 빌드. 일반 빌드와 섞이지 않게 `make clean` 뒤에 써요 |
| `make run` | 8080 포트, `www/` 루트로 실행 |
| `make test` | 단위 테스트 전체 실행 |
| `make test-debug` | ASan·UBSan을 켜고 단위 테스트 실행 |
| `make clean` | 빌드 산출물 지우기 |

## 2. 서버 켜기

```bash
./webserver -p 8080 -r ./www                          # 정적 파일만
./webserver -p 8080 -r ./www -c conf/routes.conf      # 리버스 프록시까지
```

| 인자 | 뜻 | 필수 |
| --- | --- | --- |
| `-p PORT` | 포트 번호 (1 ~ 65535) | 필수 |
| `-r ROOT` | 정적 파일 루트 폴더 | 필수 |
| `-c CONFIG` | 라우팅 설정 파일. 주면 리버스 프록시가 켜져요 | 선택 |
| `-t THREADS` | 워커 스레드 수 (1 ~ 256). 빼면 CPU 코어 수 × 2 | 선택 |

- 요청마다 접근 로그가 화면(stdout)에 한 줄씩 나와요. 파일로 남기려면 `./webserver ... > access.log`처럼 실행해요.
  ```
  2026-11-26 00:26:17 127.0.0.1 "GET /api/whoami.txt HTTP/1.1" 200 191 5ms
  ```
- `Ctrl+C`(또는 `kill`)로 끄면 새 연결은 받지 않고, 보내던 응답은 끝까지 보낸 뒤 꺼져요. 최대 5초 기다리고, `Ctrl+C`를 한 번 더 누르면 바로 꺼져요.
- 루트를 윈도우 폴더(`/mnt/c/...`, 마운트한 폴더)에 두면 파일 읽기가 느려서 처리량이 크게 떨어져요. 부하를 잴 때는 리눅스 폴더(`/tmp` 등)에 두어요.

## 3. 리버스 프록시 켜기

`/api/`로 시작하는 요청을 백엔드 두 대에 번갈아 넘기는 예예요.

**① 백엔드 두 대 띄우기** (터미널 두 개)

```bash
mkdir -p /tmp/b1/api /tmp/b2/api
echo 8001 > /tmp/b1/api/whoami.txt
echo 8002 > /tmp/b2/api/whoami.txt
cd /tmp/b1 && python3 -m http.server 8001
cd /tmp/b2 && python3 -m http.server 8002
```

**② 라우팅 설정** (`conf/routes.conf`에 예시가 있어요)

```
# 경로  백엔드1  백엔드2 ...
/api/ 127.0.0.1:8001 127.0.0.1:8002
```

- 한 줄에 규칙 하나예요. 경로 하나와 그 경로를 받을 백엔드 목록(`IPv4:포트`, 8개까지)을 공백으로 나눠 적어요.
- 요청 경로가 규칙 경로로 **시작하면** 그 규칙을 써요. 여럿 맞으면 가장 긴 경로를 골라요. (`/api/v2/`가 `/api/`보다 먼저)
- 어느 규칙에도 안 맞는 요청은 정적 파일로 처리해요.
- 경로는 바꾸지 않고 그대로 백엔드에 넘겨요. `/api/whoami.txt`는 백엔드에서도 `/api/whoami.txt`예요.
- `#`으로 시작하는 줄과 빈 줄은 건너뛰어요. 규칙은 16개까지예요.

**③ 서버 켜고 확인**

```bash
./webserver -p 8080 -r ./www -c conf/routes.conf
for i in 1 2 3 4; do curl -s localhost:8080/api/whoami.txt; done
# 8001
# 8002
# 8001
# 8002
```

- 같은 규칙의 백엔드에는 **라운드 로빈**으로 번갈아 보내요. 순번 카운터는 규칙마다 하나를 모든 워커가 같이 써서, 어느 워커가 받든 순서가 정확히 번갈아요.
- 백엔드에는 요청마다 새로 연결하고, `Connection: close`를 붙여 보내요. 백엔드가 연결을 닫으면 응답이 끝난 것으로 보고, 클라이언트 연결도 닫아요.

## 4. curl로 쓰기

```bash
curl -i localhost:8080/                       # www/index.html (디렉터리면 index.html)
curl -s localhost:8080/cat.txt                # 아스키 아트 고양이
curl -i localhost:8080/nope.html              # 404
curl -s -d 'hello' localhost:8080/echo        # POST 본문을 그대로 돌려줘요 → hello
```

| 경로 | 메서드 | 하는 일 |
| --- | --- | --- |
| `/echo` | POST | 요청 본문을 그대로 돌려줘요. GET이면 405 |
| `routes.conf`에 맞는 경로 | 아무거나 | 백엔드로 넘겨요 |
| 그 밖의 경로 | GET | `-r` 루트의 파일을 보내요 |

- `Content-Type`은 확장자로 정해요. `.html` `.css` `.js` `.txt`는 `charset=utf-8`이 붙고, `.png` `.jpg`는 이미지, 모르는 확장자는 `application/octet-stream`이에요.
- 쿼리 문자열(`?v=3`)은 파일을 찾을 때 무시해요.
- HTTP/1.1은 기본으로 연결을 유지(keep-alive)해요. 15초 동안 아무것도 안 오면 서버가 닫아요.

### 응답 코드

| 코드 | 언제 |
| --- | --- |
| 200 | 정상 |
| 400 | 요청 줄이나 헤더 형식이 틀렸을 때, HTTP/1.1인데 `Host`가 없을 때 |
| 403 | 경로에 `..`이 있거나 실제 위치가 루트 밖일 때, 읽기 권한이 없을 때, 디렉터리에 `index.html`이 없을 때 |
| 404 | 파일이 없을 때 |
| 405 | `/echo`에 GET, 파일에 POST를 보냈을 때 |
| 408 | 요청 헤더를 30초 안에 다 보내지 않았을 때 |
| 411 | POST에 `Content-Length`가 없을 때 |
| 413 | 본문이 1MB를 넘을 때 |
| 414 | 경로가 너무 길 때 |
| 431 | 요청 헤더가 8KB를 넘거나 헤더 줄이 32개를 넘을 때 |
| 501 | GET, POST가 아닌 메서드일 때 |
| 502 | 백엔드가 연결을 거절했거나, 한 바이트도 안 보내고 닫았을 때 |
| 504 | 백엔드 연결이 3초, 응답이 10초를 넘었을 때 |
| 505 | HTTP/1.0, 1.1이 아닌 버전일 때 |

## 5. 구조

```
클라이언트 ──▶ 리슨 소켓 (워커들이 공유, EPOLLEXCLUSIVE)
                 │
     ┌───────────┼───────────┐
   워커 1       워커 2   ...  워커 N        ← 스레드마다 epoll 하나
   epoll        epoll        epoll
     │  연결은 받은 워커가 끝까지 맡아요 (락 없음)
     ├─▶ 정적 파일 (www/)
     └─▶ 백엔드 8001, 8002 (라운드 로빈)
```

| 파일 | 맡는 일 |
| --- | --- |
| `src/main.c` | 인자 읽기, 라우팅 표 읽기, 리슨 소켓 열기, 워커 띄우기 |
| `src/config.c` | `-p -r -c -t` 인자 처리 |
| `src/server.c` | 워커 스레드, epoll 루프, 연결 받기·닫기, 시간 제한, 종료 신호 |
| `src/conn.c` | 연결 하나의 상태 전이 (헤더 → 본문 → 보내기) |
| `src/http.c` | 요청 파싱, keep-alive 판단 |
| `src/respond.c` | 응답 고르기 (에코, 정적 파일, 오류) |
| `src/file.c` | 경로 확인(`..`, 루트 밖 막기), MIME |
| `src/route.c` | `routes.conf` 읽기, 경로 맞추기, 라운드 로빈 |
| `src/proxy.c` | 백엔드 연결, 요청 넘기기, 64KB 중계, 502·504 |
| `src/log.c` | 접근 로그 |

자세한 설계와 결정 이유는 [DESIGN.md](DESIGN.md)와 `docs/design/step0.md` ~ `step7.md`에 있어요.

## 6. 1분 시연

명령 하나로 정적 파일(아스키 아트), 리버스 프록시 라운드 로빈, 워커 1개와 4개의 부하 비교를 차례로 보여 줘요. 서버와 백엔드는 스크립트가 직접 띄우고 꺼요.

```bash
sudo apt-get install -y wrk     # 처음 한 번 (없으면 ab, 그것도 없으면 curl로 대신해요)
make
bash scripts/demo.sh
```

- 장면 사이 쉬는 시간은 `PAUSE=5 bash scripts/demo.sh`처럼 바꿔요. 기본 3초예요.
- 8080 포트를 못 쓰면 `PORT=9090 bash scripts/demo.sh`처럼 바꿔요.
- 예전에 켜 둔 서버나 백엔드가 있으면 포트가 겹쳐요. `pkill webserver; pkill -f http.server`로 먼저 꺼요.

## 7. 부하 측정

클라우드 리눅스 2코어, 일반 빌드, 루트 `/tmp/www`, 실패는 모두 0이었어요.

| 명령 | 초당 처리 수 |
| --- | --- |
| `wrk -t4 -c100 -d10s http://127.0.0.1:8080/index.html` | 약 109,000 |
| `ab -n 20000 -c 200 http://127.0.0.1:8080/index.html` | 약 29,000 |
| 같은 `wrk`, 워커 1개 (`-t 1`) | 약 54,000 |
| 같은 `wrk`, 워커 4개 (`-t 4`) | 약 93,000 |

- `ab`는 요청마다 새로 연결해서 keep-alive를 쓰는 `wrk`보다 낮게 나와요.
- 프록시(`/api/`)는 백엔드인 `python3 -m http.server`가 병목이라 초당 1,000 ~ 2,600건이에요.
- 자세한 측정은 `docs/tests/step7.md` 3장에 있어요.

## 테스트

```bash
make test          # 단위 테스트 전체
make test-debug    # ASan·UBSan을 켜고 단위 테스트 전체
```

| 파일 | 확인하는 것 |
| --- | --- |
| `test_config.c` | 실행 인자 |
| `test_http.c` | 요청 파싱 |
| `test_file.c` | 경로 확인, MIME |
| `test_event.c` | epoll 이벤트 하나하나 (부분 읽기, 이어 보내기, 유휴 정리) |
| `test_route.c` | `routes.conf`, 긴 경로 우선, 스레드 4개 라운드 로빈 |
| `test_proxy.c` | 가짜 백엔드로 넘기기, 1MB 중계, 502·504, 끊김 정리 |
| `test_server.c` | 진짜 소켓으로 Step 2 ~ 5 동작, 워커 스레드 수 |
| `test_stable.c` | 우아한 종료, 신호, 408, 접근 로그, fd 바닥 |
| `test_smoke.c` | 빌드 확인 |
