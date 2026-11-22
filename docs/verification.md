# 검증 요약

이 문서는 Step별 검증 결과와 최종 비교에 쓸 수치를 기술하는 것을 목적으로 한다.

- 실행 명령과 상세 결과는 각 Step 문서 「5. 검증 기록」에 기술한다.
- 결과는 실제로 실행한 것만 기록한다.

## Step별 결과

| Step | 결과 | 마지막 검증일 | 비고 |
| ---- | ---- | ------------- | ---- |
| [Step 0](./steps/step-0-setup/README.md) | ⬜ | - | - |
| [Step 1](./steps/step-1-echo/README.md) | ⬜ | - | - |
| [Step 2](./steps/step-2-http-parse/README.md) | ⬜ | - | - |
| [Step 3](./steps/step-3-static/README.md) | ⬜ | - | - |
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
