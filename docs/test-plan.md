# 테스트 계획

이 문서는 테스트 분류 기준, 실행 시점, Step별 테스트 현황을 기술하는 것을 목적으로 한다.

- 테스트 케이스 본문은 각 Step 문서 「3. 테스트」에 기술한다.
- TC ID 형식: `TC-<Step>-<번호>` (예: `TC-3-02`)

## 원칙

- 테스트는 Step마다 같이 쓰고, 마지막에 몰아서 하지 않는다.
- 완료 기준 명령은 DESIGN.md에 적힌 그대로 실행한다.
- 새 모듈이 붙으면 이전 Step TC를 회귀로 다시 실행한다. (예: Step 6 → Step 3)
- 계획한 TC는 실행 전까지 통과로 세지 않는다.

## 종류

| 종류 | 대상 | 도구 |
| ---- | ---- | ---- |
| 단위 | 파서, MIME 매핑, 라우팅 매칭 | 미정 (Step 0에서 결정) |
| 기능 | 완료 기준 명령 | `curl`, `nc`, 브라우저 |
| 회귀 | 이전 Step TC 재실행 | 위와 같음 |
| 부하 | 처리량, 실패 수 | `ab`, `wrk` |
| 메모리 | 누수, 잘못된 접근 | ASan, valgrind |

## 공통 환경

- 포트 `8080`. Step 0의 인자 수용 TC는 `./www`를 사용하고, 실제 파일 시험은 Linux 컨테이너에서 `make fixtures`로 준비한 `~/mini-webserver-www`를 정적 루트로 사용한다.
- 백엔드 `8001`, `8002` (Step 6)

## Step별 현황

| Step | TC 수 | 종류 | 통과 | 실패 |
| ---- | ----- | ---- | ---- | ---- |
| [Step 0](./steps/step-0-setup/README.md) | 2 | 수동 | 2 (최종 Linux 판본) | 0 |
| [Step 1](./steps/step-1-echo/README.md) | 10 | 기능, 단위 | 10 | 0 |
| [Step 2](./steps/step-2-http-parse/README.md) | 4 | 기능, 단위 | 0 | 0 |
| [Step 3](./steps/step-3-static/README.md) | 5 | 기능, 단위 | 0 | 0 |
| [Step 4](./steps/step-4-post-keepalive/README.md) | 3 | 기능 | 0 | 0 |
| [Step 5](./steps/step-5-concurrency/README.md) | 2 | 기능, 부하 | 0 | 0 |
| [Step 6](./steps/step-6-reverse-proxy/README.md) | 5 | 기능, 단위, 회귀 | 0 | 0 |
| [Step 7](./steps/step-7-robustness/README.md) | 2 | 기능, 메모리 | 0 | 0 |
| [Step 8](./steps/step-8-final/README.md) | 2 | 부하, 회귀 | 0 | 0 |
