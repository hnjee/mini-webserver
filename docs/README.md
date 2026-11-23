# 문서 안내

이 문서는 `docs/` 아래 문서 목록과 Step 진행 중 문서를 쓰는 시점을 기술하는 것을 목적으로 한다.

## 문서 목록

| 번호 | 문서 | 알 수 있는 것 |
| ---- | ---- | ------------- |
| 1 | [progress.md](./progress.md) | Step별 진행 현황과 다음 행동 |
| 2 | [requirements.md](./requirements.md) | 요구사항 ID별 담당 Step과 충족 상태 |
| 3 | [architecture.md](./architecture.md) | 구현 구조와 설계 선택 요약 |
| 4 | [decisions.md](./decisions.md) | 결정 색인과 결정 기록 작성 형식 |
| 5 | [test-plan.md](./test-plan.md) | 테스트 분류 기준과 Step별 통과 현황 |
| 6 | [verification.md](./verification.md) | Step별 검증 결과 요약 |
| 7 | [steps/](./steps/) | Step별 진행 추적(`README.md`)과 결정 기록(`decision.md`) |
| 8 | [assignment.md](./assignment.md) | 과제 원문 |
| 9 | [technical-writing-principles.md](./technical-writing-principles.md) | 문서 작성·검수 기준 |
| 10 | [study/](./study/00-overview.md) | 배경 지식과 개념 학습 자료 |

팀 공통 설계 [DESIGN.md](../DESIGN.md)는 저장소 루트에 있다.

## Step 진행 중 쓰는 문서

| 시점 | 쓰는 문서 | 하는 일 |
| ---- | --------- | ------- |
| 시작 | 2 `requirements.md`, 7 Step `README.md` | 담당 요구사항을 확인하고 진행 체크를 시작한다 |
| 설계 | 7 Step `README.md`·`decision.md`, 3 `architecture.md`, 4 `decisions.md` | 설계 요약과 구성도를 적고, 여러 곳에 영향을 주는 선택은 `decision.md`에 기록한 뒤 색인에 올린다 |
| 테스트·구현·검증 | 7 Step `README.md` | 테스트, 구현 기록, 검증 기록을 적는다 |
| 완료 | 1 `progress.md`, 2 `requirements.md`, 5 `test-plan.md`, 6 `verification.md` | 상태, 요구사항 충족 여부, 통과 수, 검증 결과를 갱신한다 |

## Step README 구성

항목 이름만 보고 주제를 알 수 있게 쓴다. 세부는 뒤쪽 열이나 링크로 보낸다.

| 위치 | 내용 |
| ---- | ---- |
| 맨 위 표 | 상태, 요구사항, 일정, 결정 기록, 기준 |
| 1. 요구사항 | 이 Step에서 해야 할 일 |
| 2. 설계 요약 | 주제·내용·결정 표와 구성도. 이유는 `decision.md`에 둔다 |
| 3. 테스트 | TC, 주제, 종류, 실행, 기대 결과, 상태 열의 표 |
| 4. 구현 기록 | 작업, 요약, 결정 열의 표(날짜는 표 위 한 문장). 무엇을 했는지 중심으로 쓴다 |
| 5. 검증 기록 | 결과를 먼저 쓰고, 날짜·검증·결과·확인한 것 표(날짜 열 있음)와 알려진 한계를 붙인다 |
| 6. 막힌 것 / 다음 행동 | 막힌 것과 다음 Step |

## Step 폴더

| Step | 폴더 |
| ---- | ---- |
| 0 공통 준비 | [step-0-setup](./steps/step-0-setup/README.md) |
| 1 TCP 에코 서버 | [step-1-echo](./steps/step-1-echo/README.md) |
| 2 HTTP 요청 파싱 | [step-2-http-parse](./steps/step-2-http-parse/README.md) |
| 3 정적 파일 서빙 | [step-3-static](./steps/step-3-static/README.md) |
| 4 POST 본문 · keep-alive | [step-4-post-keepalive](./steps/step-4-post-keepalive/README.md) |
| 5 동시성 | [step-5-concurrency](./steps/step-5-concurrency/README.md) |
| 6 리버스 프록시 | [step-6-reverse-proxy](./steps/step-6-reverse-proxy/README.md) |
| 7 안정성 | [step-7-robustness](./steps/step-7-robustness/README.md) |
| 8 테스트와 최종 선택 | [step-8-final](./steps/step-8-final/README.md) |
