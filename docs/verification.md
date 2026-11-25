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
| [Step 4](./steps/step-4-post-keepalive/README.md) | ✅ 통과 | 2026-11-25 | 이전 독립 재확인: TC-4-01~36 통과, `make test` 675 ok줄·실패 0, 결함 주입 7개 검출. |
| [Step 5](./steps/step-5-concurrency/README.md) | ✅ 통과 | 2026-11-25 | Ubuntu 독립 ASan 빌드·단위 ok 742줄/실패 0; `nc` 중 `curl` 200; `ab -n 2000 -c 50` 실패 0·5,191.97 req/s. TC-5-08은 이전 22개 기능 ID 재사용·TC-4-17 대체·TC-3-01 Chrome 신규 실행으로 현행 기능 24개 ID 확인. |
| [Step 6](./steps/step-6-reverse-proxy/README.md) | ✅ 통과 | 2026-11-25 | TC-6-01~14 독립 검증(Mavuika). 실제 네트워크로 교대·502·504·조기 413·연결 유지·정적 회귀 확인. 설정 파서 넘침(F1) 수정 뒤 재확인 통과. 전체 `make -B test` 918 ok는 구현자 실행을 해시 일치로 재사용. |
| [Step 7](./steps/step-7-robustness/README.md) | ⬜ | - | - |
| [Step 8](./steps/step-8-final/README.md) | ⬜ | - | - |

## Step 6 뒤 워커 풀 확장

- **구현자 자체 시험 (2026-11-26):** Linux Docker에서 `make -B test` 11개·`make -B tsan` 통과, Step 3~6 기능 54건을 O2·ASan·TSan 각각 통과, `ab -n 2000 -c 50` 실패 0. [확장 기록](./extensions/worker-pool/README.md#10-구현자체-점검-기록).
- **독립 검증 (2026-11-26):** Mavuika가 변경 코드와 6개 신규 경계 시나리오를 O2·ASan·TSan에서 실행해 차단 결함을 찾지 못했다. 실제 루프의 응답 96개와 실패 종료를 추가 확인했다. 전체 기존 기능 54건·부하 시험은 독립 재실행하지 않았다. [로컬 보고서](../.agent/reviews/worker-pool-verify/report.md).
- **남은 한계:** 백엔드 교체 뒤 옛 알림 폐기는 FD 재사용 방어 시험을 통과했으나 자연 요청의 이중 알림 배치로 재현하지 못했다.

## 최종 비교용 수치

[DESIGN.md 7장](../DESIGN.md#7-최종-비교) 비교표에서 이 구현 칸에 옮길 값이다.

| 기준 | 값 | 근거 |
| ---- | -- | ---- |
| 동시성 방식 | I/O 멀티플렉싱 (`epoll` 구현) | [Step 5 결정](./steps/step-5-concurrency/decision.md) |
| Step 1~5 통과 (화요일) | | |
| 부하 `ab` (Requests/sec, 실패 수) | 독립 ASan: 5,191.97 req/s, 실패 0; 자체 -O2: 5,887.24 req/s, 실패 0 (최종 실행) | [Step 5](./steps/step-5-concurrency/README.md) |
| 부하 `wrk` | | [Step 8](./steps/step-8-final/README.md) |
| Step 6~7 통과 (수요일) | | |
| 메모리 검사 | | [Step 7](./steps/step-7-robustness/README.md) |
