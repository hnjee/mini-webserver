# 요구사항 명세

이 문서는 요구사항 ID별 담당 Step과 충족 상태를 기술하는 것을 목적으로 한다.

- 원문: [assignment.md](./assignment.md) · 팀 기준: [DESIGN.md 2·3장](../DESIGN.md#3-요구사항-id)
- 내용이 다르면 DESIGN.md를 따르며, 이 문서는 ID와 상태만 관리한다.
- 상태: ⬜ 미충족 · 🟦 일부 · ✅ 충족 (해당 Step 검증 기록이 근거)

| ID | 내용 | Step | 상태 |
| -- | ---- | ---- | ---- |
| T-01 | C + POSIX만 사용, `Makefile`/CMake, 포트·루트·라우팅을 인자나 설정 파일로 (라우팅 설정은 Step 6) | [Step 0](./steps/step-0-setup/README.md) | 🟦 |
| R-01 | `socket` → `bind` → `listen` → `accept`, 요청·응답 송수신 | [Step 1](./steps/step-1-echo/README.md) | ✅ |
| R-02 | 요청 라인·헤더(`Host`, `Content-Length`, `Connection`) 파싱 | [Step 2](./steps/step-2-http-parse/README.md) | ✅ |
| R-03 | 응답 생성: 상태 라인, 표준 헤더, 본문 | [Step 2](./steps/step-2-http-parse/README.md) | ✅ |
| R-04 | 정적 파일 서빙, MIME 매핑, 404·403·500 | [Step 3](./steps/step-3-static/README.md) | ✅ |
| R-05 | POST 본문(`Content-Length`), keep-alive | [Step 4](./steps/step-4-post-keepalive/README.md) | ✅ |
| R-06 | 동시 요청 처리 (fork / 스레드 풀 / I/O 멀티플렉싱) | [Step 5](./steps/step-5-concurrency/README.md) | ✅ |
| R-07 | 동적 요청 처리 (백엔드 전달) | [Step 6](./steps/step-6-reverse-proxy/README.md) | ⬜ |
| R-08 | 리버스 프록시 (경로 라우팅, 라운드 로빈) | [Step 6](./steps/step-6-reverse-proxy/README.md) | ⬜ |
| R-09 | `SIGPIPE` 무시, `SIGINT`·`SIGTERM` 안전 종료, 자원 회수 | [Step 7](./steps/step-7-robustness/README.md) | ⬜ |
| Q-01 | 단위 테스트(파서, MIME, 라우팅) · 기능 테스트(`curl`, 동시 접속) | [Step 2](./steps/step-2-http-parse/README.md) · [Step 3](./steps/step-3-static/README.md) · [Step 6](./steps/step-6-reverse-proxy/README.md) · [Step 8](./steps/step-8-final/README.md) | 🟦 |
| Q-02 | 부하 테스트(`ab`, `wrk`) · 메모리 검사(valgrind, ASan) | [Step 5](./steps/step-5-concurrency/README.md) · [Step 8](./steps/step-8-final/README.md) | 🟦 |
| Q-03 | 엣지 케이스: 잘못된 요청, 큰 본문·긴 URI, 백엔드 무응답, 끊김 | [Step 2](./steps/step-2-http-parse/README.md) · [Step 4](./steps/step-4-post-keepalive/README.md) · [Step 6](./steps/step-6-reverse-proxy/README.md) · [Step 7](./steps/step-7-robustness/README.md) | 🟦 |

## 기술 제한

- 언어 C, 표준 C 라이브러리 + POSIX API만 사용
- 금지: libevent, libuv, nginx, Apache, mongoose 등 완성된 HTTP·이벤트 루프 라이브러리
- `epoll`은 Linux 전용, macOS는 `kqueue` → POSIX 범위 또는 `#ifdef` 분기

## 선택 항목 (Step 7 이후 여유 있을 때)

- 포워드 프록시
- CGI
