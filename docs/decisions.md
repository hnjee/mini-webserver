# 전체 결정 색인

이 문서는 Step별 결정의 색인과 결정 기록 작성 형식을 기술하는 것을 목적으로 한다.

- 결정 본문은 각 Step의 `decision.md`에 기술한다.
- 결정 ID는 Step과 관계없이 `D-01`부터 하나씩 증가시킨다. 다음 ID: `D-09`
- 사소한 구현 세부는 결정으로 기록하지 않는다.

## 색인

| ID | Step | 결정 | 상태 |
| -- | ---- | ---- | ---- |
| D-01 | 0 | [Linux 단일 실행 환경](./steps/step-0-setup/decision.md#d-01-linux-단일-실행-환경) | 채택 |
| D-02 | 0 | [실행 인자와 테스트 구성](./steps/step-0-setup/decision.md#d-02-step-0-실행-인자와-테스트-구성) | 채택 |
| D-03 | 0 | [컨테이너 내부 시험 파일 폴더](./steps/step-0-setup/decision.md#d-03-컨테이너-내부의-전용-시험-파일-폴더) | 채택 · 구현 검증 통과 |
| D-04 | 1 | [포트 공개와 호스트 접속](./steps/step-1-echo/decision.md#d-04-포트-공개와-호스트-접속) | 채택 · 구현 검증 통과 |
| D-05 | 1 | [클라이언트 종료 뒤 쓰기 처리](./steps/step-1-echo/decision.md#d-05-클라이언트-종료-뒤-쓰기-처리는-step-1에서-최소한으로-한다) | 채택 · 구현 검증 통과 |
| D-06 | 1 | [`SO_REUSEADDR` 실험 증거](./steps/step-1-echo/decision.md#d-06-so_reuseaddr-비교-주제는-실험-증거를-남긴다) | 채택 · 구현 검증 통과 |
| D-07 | 1 | [모듈 구성과 `accept` 실패 처리](./steps/step-1-echo/decision.md#d-07-모듈-구성과-accept-실패-처리) | 채택 · 구현 검증 통과 |
| D-08 | 1 | [시험 방식과 `SO_REUSEADDR` 실험 방식](./steps/step-1-echo/decision.md#d-08-시험-방식과-so_reuseaddr-실험-방식) | 채택 · 구현 검증 통과 |

## 작성 형식

각 Step `decision.md`의 「결정」 아래에 이 형식으로 추가한다.

```markdown
### D-01. 제목

- 상태: 채택 / 임시
- 선택: 무엇을 하기로 한다
- 이유:
- 다른 선택지와 버린 이유:
- 영향: 다른 Step이나 모듈에 주는 영향
- 다시 볼 조건:
```
