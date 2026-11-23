# 전체 결정 색인

이 문서는 Step별 결정의 색인과 결정 기록 작성 형식을 기술하는 것을 목적으로 한다.

- 결정 본문은 각 Step의 `decision.md`에 기술한다.
- 결정 ID는 Step과 관계없이 `D-01`부터 하나씩 증가시킨다. 다음 ID: `D-45`
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
| D-09 | 2 | [연결별 버퍼: 상한 있는 가변 버퍼](./steps/step-2-http-parse/decision.md#d-09-연결별-버퍼는-상한이-있는-가변-크기-버퍼로-한다) | 대체됨 (D-11) |
| D-10 | 2 | [요청 상한 8KB, 초과 시 400](./steps/step-2-http-parse/decision.md#d-10-요청-상한은-8kb-초과하면-400으로-응답한다) | 채택 · 구현 검증 통과 |
| D-11 | 2 | [연결별 버퍼: 8KB 고정 크기](./steps/step-2-http-parse/decision.md#d-11-연결별-버퍼는-8kb-고정-크기로-한다) | 채택 · 구현 검증 통과 |
| D-12 | 2 | [파싱 결과: 복사 + 전체 헤더 목록](./steps/step-2-http-parse/decision.md#d-12-파싱-결과는-값을-복사해-담고-모든-헤더를-목록으로-보관한다) | 채택 · 구현 검증 통과 |
| D-13 | 2 | [HTTP/1.0·1.1 허용, `Host`는 1.1에서만 필수](./steps/step-2-http-parse/decision.md#d-13-http10과-11을-받고-host는-11에서만-필수로-한다) | 채택 · 구현 검증 통과 |
| D-14 | 2 | [모듈 구성과 연결 처리 경계](./steps/step-2-http-parse/decision.md#d-14-모듈-구성과-연결-처리-경계) | 채택 · 구현 검증 통과 |
| D-15 | 2 | [파서: 누적 바이트를 매번 다시 판정하는 상태 없는 함수](./steps/step-2-http-parse/decision.md#d-15-파서는-누적된-바이트-전체를-매번-다시-판정하는-상태-없는-함수로-한다) | 채택 · 구현 검증 통과 |
| D-16 | 2 | [요청 판정 규칙과 오류 우선순위](./steps/step-2-http-parse/decision.md#d-16-요청-판정-규칙과-오류-우선순위) | 채택 · 구현 검증 통과 |
| D-17 | 2 | [헤더 100개 상한, 초과 시 400](./steps/step-2-http-parse/decision.md#d-17-헤더는-100개까지-받고-넘으면-400으로-응답한다) | 채택 · 구현 검증 통과 |
| D-18 | 2 | [응답 생성 함수 하나, `Content-Length` 자동 계산](./steps/step-2-http-parse/decision.md#d-18-응답은-함수-하나로-만들고-content-length는-그-함수가-계산한다) | 채택 · 구현 검증 통과 |
| D-19 | 2 | [연결 하나는 요청 하나에 응답하고 반환](./steps/step-2-http-parse/decision.md#d-19-연결-하나는-완성된-요청-하나에-응답하고-돌아간다) | 채택 · 구현 검증 통과 |
| D-20 | 2 | [시험 대상과 TC 후보](./steps/step-2-http-parse/decision.md#d-20-시험-대상과-tc-후보) | 채택 · 구현 검증 통과 |
| D-21 | 2 | [응답 뒤 바로 닫기](./steps/step-2-http-parse/decision.md#d-21-응답을-보낸-뒤-연결은-바로-닫는다) | 채택 · 구현 검증 통과 |
| D-22 | 2 | [줄 바꿈 오류는 머리 완성 전에도 400](./steps/step-2-http-parse/decision.md#d-22-줄-바꿈-오류는-머리가-끝나기-전에도-바로-400으로-거절한다) | 채택 · 구현 검증 통과 |
| D-23 | 2 | [부분 `write`는 코드 검토, TC-2-06은 전 바이트 도착 확인](./steps/step-2-http-parse/decision.md#d-23-부분-write는-코드-검토로-확인하고-tc-2-06은-전-바이트-도착만-확인한다) | 채택 · 구현 검증 통과 |
| D-24 | 3 | [경로 탈출은 문자열 검사와 `realpath` 접두 비교를 둘 다 한다](./steps/step-3-static/decision.md#경로-탈출은-문자열-검사와-realpath-접두-비교를-둘-다-한다) | 채택 · 구현 검증 통과 |
| D-25 | 3 | [큰 파일은 머리를 먼저 보내고 64KB 고정 버퍼로 나눠 보낸다](./steps/step-3-static/decision.md#큰-파일은-머리를-먼저-보내고-64kb-고정-버퍼로-나눠-보낸다) | 채택 · 구현 검증 통과 |
| D-26 | 3 | [`HEAD`는 파서 규칙대로 501로 응답하되 응답 본문은 보내지 않는다](./steps/step-3-static/decision.md#head는-파서-규칙대로-501로-응답하되-응답-본문은-보내지-않는다) | 대체됨 (D-44) |
| D-27 | 3 | [오류 상황별 상태 코드](./steps/step-3-static/decision.md#오류-상황별-상태-코드) | 채택 · 구현 검증 통과 |
| D-28 | 3 | [쿼리는 버리고, 퍼센트 디코딩은 하지 않고, 슬래시 없는 디렉터리는 301로 보낸다](./steps/step-3-static/decision.md#쿼리는-버리고-퍼센트-디코딩은-하지-않고-슬래시-없는-디렉터리는-301로-보낸다) | 채택 · 구현 검증 통과 |
| D-29 | 3 | [MIME 표는 7개 확장자만 두고 나머지는 `application/octet-stream`이다](./steps/step-3-static/decision.md#mime-표는-7개-확장자만-두고-나머지는-applicationoctet-stream이다) | 채택 · 구현 검증 통과 |
| D-30 | 3 | [모듈 구성과 `http_conn` 책임 경계 (D-14 재검토)](./steps/step-3-static/decision.md#모듈-구성과-http_conn-책임-경계-d-14-재검토) | 채택 · 구현 검증 통과 |
| D-31 | 3 | [응답은 머리 쓰기를 따로 두고 301·403·404·500과 `Location`을 더한다 (D-18 갱신, D-40으로 보완)](./steps/step-3-static/decision.md#응답은-머리-쓰기를-따로-두고-301403404500과-location을-더한다-d-18-갱신-d-40으로-보완) | 채택 · 구현 검증 통과 |
| D-32 | 3 | [요청 대상 판정 규칙과 순서](./steps/step-3-static/decision.md#요청-대상-판정-규칙과-순서) | 채택 · 구현 검증 통과 |
| D-33 | 3 | [파일 본문 전송, 파일 fd 소유, 실패 동작](./steps/step-3-static/decision.md#파일-본문-전송-파일-fd-소유-실패-동작) | 채택 · 구현 검증 통과 |
| D-34 | 3 | [`HEAD` 요청은 받은 바이트의 앞 5바이트로 판별한다](./steps/step-3-static/decision.md#head-요청은-받은-바이트의-앞-5바이트로-판별한다) | 대체됨 (D-44) |
| D-35 | 3 | [MIME 판정 함수 계약](./steps/step-3-static/decision.md#mime-판정-함수-계약) | 채택 · 구현 검증 통과 |
| D-36 | 3 | [시험 대상과 TC 후보](./steps/step-3-static/decision.md#시험-대상과-tc-후보) | 채택 · 구현 검증 통과 |
| D-37 | 3 | [Step 3에서 `POST`는 `GET`과 똑같이 파일을 보낸다 (Step 4 전까지의 임시 동작)](./steps/step-3-static/decision.md#step-3에서-post는-get과-똑같이-파일을-보낸다-step-4-전까지의-임시-동작) | 채택 · 구현 검증 통과 |
| D-38 | 3 | [일반 파일이 아닌 대상(FIFO·장치·소켓)은 403으로 응답한다](./steps/step-3-static/decision.md#일반-파일이-아닌-대상fifo장치소켓은-403으로-응답한다) | 채택 · 구현 검증 통과 |
| D-39 | 3 | [하위 폴더 시험 파일 `sub/index.html`과 빈 폴더 `empty/`를 `make fixtures`에 더한다](./steps/step-3-static/decision.md#하위-폴더-시험-파일-subindexhtml과-빈-폴더-empty를-make-fixtures에-더한다) | 채택 · 구현 검증 통과 |
| D-40 | 3 | [메모리 본문 응답은 상태 응답 함수 하나가 본문과 길이를 만든다 (설계 검토 C2)](./steps/step-3-static/decision.md#메모리-본문-응답은-상태-응답-함수-하나가-본문과-길이를-만든다-설계-검토-c2) | 채택 · 구현 검증 통과 |
| D-41 | 3 | [너무 긴 경로는 400으로 응답한다](./steps/step-3-static/decision.md#너무-긴-경로는-400으로-응답한다) | 채택 · 구현 검증 통과 |
| D-42 | 3 | [301 `Location`에는 쿼리를 붙이지 않는다](./steps/step-3-static/decision.md#301-location에는-쿼리를-붙이지-않는다) | 채택 · 구현 검증 통과 |
| D-43 | 3 | [MIME은 요청한 주소의 마지막 이름으로 정한다](./steps/step-3-static/decision.md#mime은-요청한-주소의-마지막-이름으로-정한다) | 채택 · 구현 검증 통과 |
| D-44 | 3 | [`HEAD`는 Step 2처럼 501과 본문을 그대로 보낸다 (D-26 대체)](./steps/step-3-static/decision.md#head는-step-2처럼-501과-본문을-그대로-보낸다-d-26-대체) | 채택 · 구현 검증 통과 |

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
