# mini-webserver

이 문서는 C와 POSIX 소켓으로 만드는 웹 서버(+ 리버스 프록시) 저장소의 문서 구성과 탐색 경로를 기술하는 것을 목적으로 한다.

## 어디부터 보나

```mermaid
flowchart LR
    A["DESIGN.md<br/>팀 공통 설계"] --> B["docs/progress.md<br/>전체 진행 현황"] --> C["docs/steps/step-N/<br/>Step별 추적·결정"]
```

## 폴더 구조

```text
mini-webserver/
├── README.md                  이 문서
├── DESIGN.md                  팀 공통 설계 (Step 0~8, 완료 기준)
└── docs/
    ├── README.md              문서 안내 (찾고 싶은 것으로 찾기)
    ├── assignment.md          과제 원문
    ├── technical-writing-principles.md   문서 작성 기준
    ├── progress.md            전체 진행 현황
    ├── requirements.md        요구사항 명세 (ID별 상태)
    ├── architecture.md        전체 설계 (이 구현의 선택 요약)
    ├── test-plan.md           테스트 계획
    ├── verification.md        검증 요약
    ├── decisions.md           전체 결정 색인
    ├── steps/
    │   └── step-N-이름/
    │       ├── README.md      Step 상황 추적 (요구사항·설계·테스트·구현·검증)
    │       └── decision.md    Step 결정 기록
    └── study/                 학습 자료
```

## 문서 역할

| 범위 | 무엇을 | 어디에 |
| ---- | ------ | ------ |
| 전체 | 진행 현황 | [progress.md](./docs/progress.md) |
| 전체 | 요구사항 · 설계 · 테스트 · 검증 | [requirements.md](./docs/requirements.md) · [architecture.md](./docs/architecture.md) · [test-plan.md](./docs/test-plan.md) · [verification.md](./docs/verification.md) |
| 전체 | 결정 색인 | [decisions.md](./docs/decisions.md) |
| Step | 상황 추적 | `docs/steps/step-N-이름/README.md` |
| Step | 결정 기록 | `docs/steps/step-N-이름/decision.md` |
