# 검증 요약

이 문서는 Step별 검증 결과와 최종 비교에 쓸 수치를 기술하는 것을 목적으로 한다.

- 실행 명령과 상세 결과는 각 Step 문서 「5. 검증 기록」에 기술한다.
- 결과는 실제로 실행한 것만 기록한다.

## Step별 결과

| Step | 결과 | 마지막 검증일 | 비고 |
| ---- | ---- | ------------- | ---- |
| [Step 0](./steps/step-0-setup/README.md) | ✅ 통과 | 2026-11-23 | 최종 판본 TC-0-01/02·시험 폴더 경로 독립 실행 통과. 22개 단위 검사·ASan·getopt는 변경 없는 코드의 이전 증거 재사용. |
| [Step 1](./steps/step-1-echo/README.md) | ✅ 통과 | 2026-11-23 | TC-1-01~10 독립 실행 통과, 단위 검사 29개. 부분 `write`·`EINTR`는 시험이 아닌 코드 검토. |
| [Step 2](./steps/step-2-http-parse/README.md) | ✅ 통과 | 2026-11-23 | TC-2-01~11 독립 실행 통과, 단위 검사 156개(전체 `make test`). 30만 건 참조 모델 비교 불일치 0. 부분 `write`·`EINTR`·`read` 오류 경로는 시험이 아닌 코드 검토. |
| [Step 3](./steps/step-3-static/README.md) | ✅ 통과 | 2026-11-23 | TC-3-01~19 독립 실행 통과(Mavuika, 최종 소스 해시 일치), 단위 검사 412건(전체 `make test`, ASan, 비root), 빌드 경고 0건. 변이 시험 23개 중 18개를 시험이 잡았고 나머지는 주입 시험·설계상 이중 방어로 확인. 머리 전송 뒤 실패 경로는 `read`·`close` 주입 시험으로 확인. 스크린샷은 눈으로 판정하지 못하고 DOM 계산 스타일·JS 실행·이미지 크기로 판정. |
| [Step 4](./steps/step-4-post-keepalive/README.md) | ⬜ | - | - |
| [Step 5](./steps/step-5-concurrency/README.md) | ⬜ | - | - |
| [Step 6](./steps/step-6-reverse-proxy/README.md) | ⬜ | - | - |
| [Step 7](./steps/step-7-robustness/README.md) | ⬜ | - | - |
| [Step 8](./steps/step-8-final/README.md) | ⬜ | - | - |

## 최종 비교용 수치

[DESIGN.md 7장](../DESIGN.md#7-최종-비교) 비교표에서 이 구현 칸에 옮길 값이다.

| 기준 | 값 | 근거 |
| ---- | -- | ---- |
| 동시성 방식 | | [Step 5 결정](./steps/step-5-concurrency/decision.md) |
| Step 1~5 통과 (화요일) | | |
| 부하 `ab` (Requests/sec, 실패 수) | | [Step 5](./steps/step-5-concurrency/README.md) |
| 부하 `wrk` | | [Step 8](./steps/step-8-final/README.md) |
| Step 6~7 통과 (수요일) | | |
| 메모리 검사 | | [Step 7](./steps/step-7-robustness/README.md) |
