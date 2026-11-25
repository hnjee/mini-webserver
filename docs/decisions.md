# 전체 결정 색인

이 문서는 Step별 결정의 색인과 결정 기록 작성 형식을 기술하는 것을 목적으로 한다.

- 결정 본문은 각 Step의 `decision.md`에 기술한다.
- 결정 ID는 Step과 관계없이 `D-01`부터 하나씩 증가시킨다. 다음 ID: `D-60`
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
| D-09 | 2 | [요청 상한 8KB, 초과 시 400](./steps/step-2-http-parse/decision.md#d-09-요청-상한은-8kb-초과하면-400으로-응답한다) | 채택 · 구현 검증 통과 · 응답 코드는 D-42로 갱신 |
| D-10 | 2 | [연결별 버퍼: 8KB 고정 크기](./steps/step-2-http-parse/decision.md#d-10-연결별-버퍼는-8kb-고정-크기로-한다) | 채택 · 구현 검증 통과 |
| D-11 | 2 | [파싱 결과: 복사 + 전체 헤더 목록](./steps/step-2-http-parse/decision.md#d-11-파싱-결과는-값을-복사해-담고-모든-헤더를-목록으로-보관한다) | 채택 · 구현 검증 통과 |
| D-12 | 2 | [HTTP/1.0·1.1 허용, `Host`는 1.1에서만 필수](./steps/step-2-http-parse/decision.md#d-12-http10과-11을-받고-host는-11에서만-필수로-한다) | 채택 · 구현 검증 통과 |
| D-13 | 2 | [모듈 구성과 연결 처리 경계](./steps/step-2-http-parse/decision.md#d-13-모듈-구성과-연결-처리-경계) | 채택 · 구현 검증 통과 |
| D-14 | 2 | [파서: 누적 바이트를 매번 다시 판정하는 상태 없는 함수](./steps/step-2-http-parse/decision.md#d-14-파서는-누적된-바이트-전체를-매번-다시-판정하는-상태-없는-함수로-한다) | 채택 · 구현 검증 통과 |
| D-15 | 2 | [요청 판정 규칙과 오류 우선순위](./steps/step-2-http-parse/decision.md#d-15-요청-판정-규칙과-오류-우선순위) | 채택 · 구현 검증 통과 |
| D-16 | 2 | [응답 생성 함수 하나, `Content-Length` 자동 계산](./steps/step-2-http-parse/decision.md#d-16-응답은-함수-하나로-만들고-content-length는-그-함수가-계산한다) | 채택 · 구현 검증 통과 |
| D-17 | 2 | [연결 하나는 요청 하나에 응답하고 반환](./steps/step-2-http-parse/decision.md#d-17-연결-하나는-완성된-요청-하나에-응답하고-돌아간다) | 채택 · 구현 검증 통과 |
| D-18 | 2 | [응답 뒤 바로 닫기](./steps/step-2-http-parse/decision.md#d-18-응답을-보낸-뒤-연결은-바로-닫는다) | 채택 · 구현 검증 통과 |
| D-19 | 2 | [줄 바꿈 오류는 머리 완성 전에도 400](./steps/step-2-http-parse/decision.md#d-19-줄-바꿈-오류는-머리가-끝나기-전에도-바로-400으로-거절한다) | 채택 · 구현 검증 통과 |
| D-20 | 2 | [부분 `write`는 코드 검토, TC-2-06은 전 바이트 도착 확인](./steps/step-2-http-parse/decision.md#d-20-부분-write는-코드-검토로-확인하고-tc-2-06은-전-바이트-도착만-확인한다) | 채택 · 구현 검증 통과 |
| D-21 | 3 | [경로 탈출은 문자열 검사와 `realpath` 접두 비교를 둘 다 한다](./steps/step-3-static/decision.md#d-21-경로-탈출은-문자열-검사와-realpath-접두-비교를-둘-다-한다) | 채택 · 구현 검증 통과 |
| D-22 | 3 | [큰 파일은 머리를 먼저 보내고 64KB 고정 버퍼로 나눠 보낸다](./steps/step-3-static/decision.md#d-22-큰-파일은-머리를-먼저-보내고-64kb-고정-버퍼로-나눠-보낸다) | 채택 · 구현 검증 통과 |
| D-23 | 3 | [오류 상황별 상태 코드](./steps/step-3-static/decision.md#d-23-오류-상황별-상태-코드) | 채택 · 구현 검증 통과 |
| D-24 | 3 | [쿼리는 버리고, 퍼센트 디코딩은 하지 않고, 슬래시 없는 디렉터리는 301로 보낸다](./steps/step-3-static/decision.md#d-24-쿼리는-버리고-퍼센트-디코딩은-하지-않고-슬래시-없는-디렉터리는-301로-보낸다) | 채택 · 구현 검증 통과 |
| D-25 | 3 | [MIME 표는 7개 확장자만 두고 나머지는 `application/octet-stream`이다](./steps/step-3-static/decision.md#d-25-mime-표는-7개-확장자만-두고-나머지는-applicationoctet-stream이다) | 채택 · 구현 검증 통과 |
| D-26 | 3 | [모듈 구성과 `http_conn` 책임 경계 (D-13 재검토)](./steps/step-3-static/decision.md#d-26-모듈-구성과-http_conn-책임-경계-d-13-재검토) | 채택 · 구현 검증 통과 |
| D-27 | 3 | [응답은 머리 쓰기를 따로 두고 301·403·404·500과 `Location`을 더한다 (D-16 갱신, D-33으로 보완)](./steps/step-3-static/decision.md#d-27-응답은-머리-쓰기를-따로-두고-301403404500과-location을-더한다-d-16-갱신-d-33으로-보완) | 채택 · 구현 검증 통과 |
| D-28 | 3 | [요청 대상 판정 규칙과 순서](./steps/step-3-static/decision.md#d-28-요청-대상-판정-규칙과-순서) | 채택 · 구현 검증 통과 |
| D-29 | 3 | [파일 본문 전송, 파일 fd 소유, 실패 동작](./steps/step-3-static/decision.md#d-29-파일-본문-전송-파일-fd-소유-실패-동작) | 채택 · 구현 검증 통과 |
| D-30 | 3 | [Step 3에서 `POST`는 `GET`과 똑같이 파일을 보낸다 (Step 4 전까지의 임시 동작)](./steps/step-3-static/decision.md#d-30-step-3에서-post는-get과-똑같이-파일을-보낸다-step-4-전까지의-임시-동작) | 채택 · 구현 검증 통과 |
| D-31 | 3 | [일반 파일이 아닌 대상(FIFO·장치·소켓)은 403으로 응답한다](./steps/step-3-static/decision.md#d-31-일반-파일이-아닌-대상fifo장치소켓은-403으로-응답한다) | 채택 · 구현 검증 통과 |
| D-32 | 3 | [하위 폴더 시험 파일 `sub/index.html`과 빈 폴더 `empty/`를 `make fixtures`에 더한다](./steps/step-3-static/decision.md#d-32-하위-폴더-시험-파일-subindexhtml과-빈-폴더-empty를-make-fixtures에-더한다) | 채택 · 구현 검증 통과 |
| D-33 | 3 | [메모리 본문 응답은 상태 응답 함수 하나가 본문과 길이를 만든다 (설계 검토 C2)](./steps/step-3-static/decision.md#d-33-메모리-본문-응답은-상태-응답-함수-하나가-본문과-길이를-만든다-설계-검토-c2) | 채택 · 구현 검증 통과 |
| D-34 | 3 | [MIME은 요청한 주소의 마지막 이름으로 정한다](./steps/step-3-static/decision.md#d-34-mime은-요청한-주소의-마지막-이름으로-정한다) | 채택 · 구현 검증 통과 |
| D-35 | 3 | [`HEAD`는 Step 2처럼 501과 본문을 그대로 보낸다 (본문 생략 안 대체)](./steps/step-3-static/decision.md#d-35-head는-step-2처럼-501과-본문을-그대로-보낸다-본문-생략-안-대체) | 채택 · 구현 검증 통과 |
| D-36 | 4 | [파일 주소로 온 `POST`와 `GET /echo`는 405로 응답한다 (D-30 대체)](./steps/step-4-post-keepalive/decision.md#d-36-파일-주소로-온-post와-get-echo는-405로-응답한다-d-30-대체) | 채택 |
| D-37 | 4 | [오류 응답 뒤에도 요청의 끝을 알면 연결을 유지한다](./steps/step-4-post-keepalive/decision.md#d-37-오류-응답-뒤에도-요청의-끝을-알면-연결을-유지한다) | 채택 |
| D-38 | 4 | [HTTP/1.0도 `Connection: keep-alive`를 보내면 연결을 유지한다](./steps/step-4-post-keepalive/decision.md#d-38-http10도-connection-keep-alive를-보내면-연결을-유지한다) | 채택 |
| D-39 | 4 | [본문은 모아 두지 않고 조각으로 흘려보낸다](./steps/step-4-post-keepalive/decision.md#d-39-본문은-모아-두지-않고-조각으로-흘려보낸다) | 채택 |
| D-40 | 4 | [타임아웃은 소켓 옵션 `SO_RCVTIMEO`로 건다](./steps/step-4-post-keepalive/decision.md#d-40-타임아웃은-소켓-옵션-so_rcvtimeo로-건다) | 채택 |
| D-41 | 4 | [타임아웃은 5초로 한다](./steps/step-4-post-keepalive/decision.md#d-41-타임아웃은-5초로-한다) | 채택 |
| D-42 | 4 | [요청 머리가 너무 크면 431로 응답한다 (D-09 갱신)](./steps/step-4-post-keepalive/decision.md#d-42-요청-머리가-너무-크면-431로-응답한다-d-09-갱신) | 채택 |
| D-43 | 4 | [URI가 2KB를 넘으면 414로 응답한다](./steps/step-4-post-keepalive/decision.md#d-43-uri가-2kb를-넘으면-414로-응답한다) | 채택 |
| D-44 | 4 | [본문이 1MB를 넘으면 413으로 응답한다](./steps/step-4-post-keepalive/decision.md#d-44-본문이-1mb를-넘으면-413으로-응답한다) | 채택 |
| D-45 | 4 | [연결을 닫는 오류 응답 뒤에는 남은 데이터를 잠깐 받아 버린 뒤 닫는다](./steps/step-4-post-keepalive/decision.md#d-45-연결을-닫는-오류-응답-뒤에는-남은-데이터를-잠깐-받아-버린-뒤-닫는다) | 채택 |
| D-46 | 4 | [연결 반복은 `http_conn`이 맡고 `main`은 받고 닫기만 한다](./steps/step-4-post-keepalive/decision.md#d-46-연결-반복은-http_conn이-맡고-main은-받고-닫기만-한다) | 채택 |
| D-47 | 4 | [8KB 버퍼 하나를 쓰고 남은 바이트는 앞으로 옮긴다 (D-14 갱신)](./steps/step-4-post-keepalive/decision.md#d-47-8kb-버퍼-하나를-쓰고-남은-바이트는-앞으로-옮긴다-d-14-갱신) | 채택 |
| D-48 | 4 | [요청 판정 순서: 크기 → 형식 → 본문 길이 (D-15 갱신)](./steps/step-4-post-keepalive/decision.md#d-48-요청-판정-순서-크기--형식--본문-길이-d-15-갱신) | 채택 |
| D-49 | 4 | [본문 길이는 `Content-Length` 숫자 하나로만 정한다](./steps/step-4-post-keepalive/decision.md#d-49-본문-길이는-content-length-숫자-하나로만-정한다) | 채택 |
| D-50 | 4 | [`/echo` 판정과 본문 처리 순서](./steps/step-4-post-keepalive/decision.md#d-50-echo-판정과-본문-처리-순서) | 채택 |
| D-51 | 4 | [연결 유지는 응답 전에 한 번 정하고 `Connection` 헤더와 맞춘다](./steps/step-4-post-keepalive/decision.md#d-51-연결-유지는-응답-전에-한-번-정하고-connection-헤더와-맞춘다) | 채택 |
| D-52 | 4 | [응답 함수는 `Allow`·`Connection`을 추가 머리 묶음으로 받는다 (D-27·D-33 갱신)](./steps/step-4-post-keepalive/decision.md#d-52-응답-함수는-allowconnection을-추가-머리-묶음으로-받는다-d-27d-33-갱신) | 채택 |
| D-53 | 4 | [타임아웃은 연결 시작 때 걸고, 기다린 위치로 결과를 나눈다](./steps/step-4-post-keepalive/decision.md#d-53-타임아웃은-연결-시작-때-걸고-기다린-위치로-결과를-나눈다) | 채택 |
| D-54 | 4 | [닫기 전 정리는 총 5초까지, 바이트 상한 없이 한다 (D-45 세부)](./steps/step-4-post-keepalive/decision.md#d-54-닫기-전-정리는-총-5초까지-바이트-상한-없이-한다-d-45-세부) | 채택 |
| D-55 | 5 | [I/O 멀티플렉싱을 동시성 방식으로 선택한다](./steps/step-5-concurrency/decision.md#d-55-io-멀티플렉싱을-동시성-방식으로-선택한다) | 채택 |
| D-56 | 5 | [`epoll` 레벨 트리거와 공평한 처리](./steps/step-5-concurrency/decision.md#d-56-epoll-레벨-트리거로-기다리고-한-차례에-정한-만큼만-처리한다) | 구현 내부 선택 · 영향 검증 통과 |
| D-57 | 5 | [연결 상태와 모듈 분담](./steps/step-5-concurrency/decision.md#d-57-연결-상태를-구조체로-옮기고-루프와-연결-처리를-나눈다-d-46-대체) | 구현 내부 선택 · 영향 검증 통과 |
| D-58 | 5 | [연결별 출력 버퍼와 나눠 보내기](./steps/step-5-concurrency/decision.md#d-58-응답은-연결별-출력-버퍼에-만들고-나눠-보낸다) | 구현 내부 선택 · 영향 검증 통과 |
| D-59 | 5 | [읽기·쓰기 진행 없는 5초 제한](./steps/step-5-concurrency/decision.md#d-59-읽기쓰기-진행이-없으면-5초-뒤-닫는다-d-40d-53의-시간-측정-변경) | 정책·준비 우선 순서 사용자 선택 · 구현 영향 검증 통과 |

## 작성 형식

각 Step `decision.md`의 「결정」 아래에 이 형식으로 추가한다. 짧게 쓴다.

```markdown
### D-01. 제목
- 선택: 무엇을 하기로 한다 (핵심어만 **볼드**)
- 이유: 
- 버린 선택지: 핵심 1~2개와 이유
- 영향·다시 볼 조건: 있을 때만
```

- 불렛은 3~5개, 한 불렛은 한두 문장으로 쓴다. 줄글은 쓰지 않는다.
- 볼드는 결정마다 2~3곳 이내로 쓴다.
- **설계와 맥락상의 선택**을 쓴다. 함수 시그니처, 버퍼 크기, `errno` 처리 같은 구현 세부와 시험 목록은 코드·README에 둔다.
- 결과가 달라지는 조건(오류 매핑, 우선순위)은 줄이지 않는다.
- 임시 결정은 제목 뒤에 `(임시)`로 표시한다. 채택한 결정은 표시하지 않는다.
- 번호는 빈틈없이 유지한다. 대체·합쳐진 결정은 번호를 따로 두지 않고, 남은 결정의 문장으로 적는다(예: "가변 버퍼 안을 대체"). 번호를 당길 때는 문서·코드 주석의 참조와 링크를 한꺼번에 바꾸고 검사한다.
