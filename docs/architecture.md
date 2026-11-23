# 전체 설계

이 문서는 이 구현의 전체 구조와 Step별 설계 선택을 기술하는 것을 목적으로 한다.

- 공통 구조(요청 흐름, 모듈 역할): [DESIGN.md 4장](../DESIGN.md#4-전체-구조)
- 이 문서는 **이 구현만의 선택**을 요약하며, 자세한 이유는 각 Step 결정 기록에 기술한다.

## 선택 요약

| 항목 | 선택 | 결정 기록 |
| ---- | ---- | --------- |
| 실행 환경 | Ubuntu 24.04 Docker Linux, 비root `jungle` | [Step 0](./steps/step-0-setup/decision.md) |
| 소스·테스트 폴더 구성 | `src/`, `tests/` 분리 | [Step 0](./steps/step-0-setup/decision.md) |
| 실행 인자 파싱 | 별도 `getopt` 모듈 (`-p`, `-r`; `-c`는 Step 6) | [Step 0](./steps/step-0-setup/decision.md) |
| 시험 파일 | 컨테이너 내부 전용 폴더 재사용 (Step 0 검증 통과) | [Step 0](./steps/step-0-setup/decision.md) |
| 에코 서버 구성 | `listener`·`echo` 모듈 분리, `accept` 반복은 `main.c`, 순차 처리, `SIGPIPE` 무시 | [Step 1](./steps/step-1-echo/decision.md) |
| 연결별 버퍼 관리 | 8KB 고정 버퍼, 초과 시 400 | [Step 2](./steps/step-2-http-parse/decision.md) (D-10, D-11) |
| HTTP 버전·`Host` 규칙 | 1.0·1.1 허용, `Host`는 1.1에서만 필수 | [Step 2](./steps/step-2-http-parse/decision.md) (D-13) |
| 파싱 결과 구조 | 값 복사 + 전체 헤더 목록 (헤더 수 상한은 설계) | [Step 2](./steps/step-2-http-parse/decision.md) |
| 경로 탈출 방지 · 큰 파일 전송 | 문자열 검사 + `realpath` 접두 비교, 64KB 고정 버퍼 조각 전송 (구현 검증 통과) | [Step 3](./steps/step-3-static/decision.md) (D-24, D-25) |
| 남은 바이트 처리 · 타임아웃 | 미정 | [Step 4](./steps/step-4-post-keepalive/decision.md) |
| 동시성 방식 | 미정 | [Step 5](./steps/step-5-concurrency/decision.md) |
| 라우팅 설정 형식 · 백엔드 연결 재사용 | 미정 | [Step 6](./steps/step-6-reverse-proxy/decision.md) |
| 종료 방식 | 미정 | [Step 7](./steps/step-7-robustness/decision.md) |

## 모듈 구성

Step을 진행하며 실제로 만든 모듈만 추가한다.

| 모듈 | 파일 | 역할 | 추가된 Step |
| ---- | ---- | ---- | ----------- |
| 실행 인자 | `src/options.c`, `src/options.h` | 포트·정적 루트 인자 검사 | 0 |
| 진입점 | `src/main.c` | 인자 확인, `SIGPIPE` 무시, `accept` 반복, 연결 소켓 닫기, 연결 처리는 `http_conn_serve` 호출 | 0, 1, 2 |
| 듣는 소켓 | `src/listener.c`, `src/listener.h` | `socket`·`SO_REUSEADDR`·`bind`·`listen` | 1 |
| 에코 | `src/echo.c`, `src/echo.h` | 연결 하나의 읽기·쓰기 에코 (Step 2부터 서버는 호출하지 않고 회귀 시험용으로 유지) | 1 |
| HTTP 파서 | `src/http_parser.c`, `src/http_parser.h` | 받은 바이트를 요청 구조로 판정(불완전·완료·400·501·505), 소켓 I/O 없음 | 2 |
| 응답 생성 | `src/http_response.c`, `src/http_response.h` | 상태·본문으로 HTTP 응답을 만들어 끝까지 씀 (`Content-Length` 자동 계산). Step 3에서 머리만 쓰는 함수(파일 응답), 상태 응답 함수(오류·301), 301·403·404·500·`Location` 추가 | 2, 3 |
| 연결 처리 | `src/http_conn.c`, `src/http_conn.h` | 연결 하나: 8KB 버퍼에 읽기, 파서 호출, 결과를 상태 코드로 바꿔 응답, 파일 fd 닫기 | 2, 3 |
| 정적 파일 | `src/static_file.c`, `src/static_file.h` | 요청 대상 판정(경로 탈출 방지·디렉터리·`index.html`), 파일 열기, 64KB 조각 전송 | 3 |
| MIME | `src/mime.c`, `src/mime.h` | 확장자로 `Content-Type` 결정(7개, 나머지 `octet-stream`) | 3 |
| 쓰기 보조 | `src/io.c`, `src/io.h` | 부분 `write`를 이어 쓰는 `io_write_all` | 2 |
