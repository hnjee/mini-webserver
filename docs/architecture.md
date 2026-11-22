# 전체 설계

이 문서는 이 구현의 전체 구조와 Step별 설계 선택을 기술하는 것을 목적으로 한다.

- 공통 구조(요청 흐름, 모듈 역할): [DESIGN.md 4장](../DESIGN.md#4-전체-구조)
- 이 문서는 **이 구현만의 선택**을 요약하며, 자세한 이유는 각 Step 결정 기록에 기술한다.

## 선택 요약

| 항목 | 선택 | 결정 기록 |
| ---- | ---- | --------- |
| 소스·테스트 폴더 구성 | 미정 | [Step 0](./steps/step-0-setup/decision.md) |
| 실행 인자 파싱 | 미정 | [Step 0](./steps/step-0-setup/decision.md) |
| 연결별 버퍼 관리 | 미정 | [Step 2](./steps/step-2-http-parse/decision.md) |
| 파싱 결과 구조 | 미정 | [Step 2](./steps/step-2-http-parse/decision.md) |
| 경로 탈출 방지 · 큰 파일 전송 | 미정 | [Step 3](./steps/step-3-static/decision.md) |
| 남은 바이트 처리 · 타임아웃 | 미정 | [Step 4](./steps/step-4-post-keepalive/decision.md) |
| 동시성 방식 | 미정 | [Step 5](./steps/step-5-concurrency/decision.md) |
| 라우팅 설정 형식 · 백엔드 연결 재사용 | 미정 | [Step 6](./steps/step-6-reverse-proxy/decision.md) |
| 종료 방식 | 미정 | [Step 7](./steps/step-7-robustness/decision.md) |

## 모듈 구성

Step을 진행하며 실제로 만든 모듈만 추가한다.

| 모듈 | 파일 | 역할 | 추가된 Step |
| ---- | ---- | ---- | ----------- |
