# 전체 진행 현황

이 문서는 Step 0~8의 전체 진행 상태와 다음 행동을 기술하는 것을 목적으로 한다.

상태: ⬜ 시작 전 · 🟦 진행 중 · ✅ 완료 · ⛔ 막힘

```mermaid
flowchart LR
    S0["0 공통 준비"] --> S1["1 TCP 에코"] --> S2["2 HTTP 파싱"] --> S3["3 정적 파일"] --> S4["4 POST·keep-alive"]
    S4 --> S5["5 동시성"] --> S6["6 리버스 프록시"] --> S7["7 안정성"] --> S8["8 테스트·선택"]

    classDef todo fill:#F1EFE8,stroke:#888780,color:#2C2C2A
    classDef doing fill:#E6F1FB,stroke:#185FA5,color:#042C53
    classDef done fill:#EAF3DE,stroke:#3B6D11,color:#173404
    classDef blocked fill:#FCEBEB,stroke:#A32D2D,color:#501313
    class S0,S1,S2,S3 done
    class S4,S5,S6 done
    class S7,S8 todo
```

| Step | 내용 | 요구사항 | 일정 | 상태 | 문서 |
| ---- | ---- | -------- | ---- | ---- | ---- |
| 0 | 공통 준비 | T-01 (라우팅 설정은 Step 6) | 11/21 (토) | ✅ 완료 | [Step 0](./steps/step-0-setup/README.md) · [결정](./steps/step-0-setup/decision.md) |
| 1 | TCP 에코 서버 | R-01 | 11/21 (토) | ✅ 완료 | [Step 1](./steps/step-1-echo/README.md) · [결정](./steps/step-1-echo/decision.md) |
| 2 | HTTP 요청 파싱 + 고정 응답 | R-02, R-03, Q-03 | 11/23 (월) | ✅ 완료 | [Step 2](./steps/step-2-http-parse/README.md) · [결정](./steps/step-2-http-parse/decision.md) |
| 3 | 정적 파일 서빙 | R-04 | 11/23 (월) | ✅ 완료 | [Step 3](./steps/step-3-static/README.md) · [결정](./steps/step-3-static/decision.md) |
| 4 | POST 본문 + keep-alive | R-05, Q-03 | 11/24 (화) | ✅ 완료 | [Step 4](./steps/step-4-post-keepalive/README.md) · [결정](./steps/step-4-post-keepalive/decision.md) |
| 5 | 동시성 | R-06, Q-02 | 11/24 (화) | ✅ 완료 | [Step 5](./steps/step-5-concurrency/README.md) · [결정](./steps/step-5-concurrency/decision.md) |
| 6 | 리버스 프록시 (동적 요청 처리 포함) | R-07, R-08, Q-03 | 11/25 (수) 오전 | ✅ 완료 | [Step 6](./steps/step-6-reverse-proxy/README.md) · [결정](./steps/step-6-reverse-proxy/decision.md) |
| 7 | 안정성 | R-09, Q-03 | 11/25 (수) 오후 | ⬜ 시작 전 | [Step 7](./steps/step-7-robustness/README.md) · [결정](./steps/step-7-robustness/decision.md) |
| 8 | 테스트와 최종 선택 | Q-01, Q-02 | 11/25 (수) 오후 | ⬜ 시작 전 | [Step 8](./steps/step-8-final/README.md) · [결정](./steps/step-8-final/decision.md) |

## 지금

- 완료: Step 6 리버스 프록시. TC-6-01~14 독립 검증 통과, 설정 파서 넘침(F1)은 수정 뒤 재확인 통과. [검증 요약](./verification.md).
- 후속 확장: [epoll + 작업 큐 + 워커 풀](./extensions/worker-pool/README.md) 구현·독립 코드 검증 통과. 세부 설계 선택은 사용자 채택 전 `임시`이며 Step 5·6 완료 기록과 별개다.
- 다음: Step 7 안정성. 시작 실패 경로의 `realpath` 누수(4097바이트)를 TC-7-02에서 함께 정리한다.
- 막힌 것: 없음.

## 기준선

- 화요일(11/24): Step 1~5 통과 현황과 `ab` 처리량을 [검증 요약](./verification.md)에 기록
- 수요일(11/25) 정오: Step 6 미통과 구현은 최종 후보에서 제외 ([DESIGN.md 6장](../DESIGN.md#6-일정))
