[⬅ overview](./00-overview.md)

---

# 5. HTTP/1.1 메시지
> - 요청과 응답의 구조: 시작 줄, 헤더, 빈 줄, 본문
> - 메서드(GET, POST)와 상태 코드(2xx, 4xx, 5xx)
> - 핵심 헤더: Host, Content-Type, Content-Length, Connection
> - 경계 식별: \r\n\r\n, Content-Length 기반 본문 길이 (chunked는 개념만)
> - keep-alive: 연결 하나로 여러 요청 처리하기

---

## 요청과 응답의 구조
- HTTP 메시지 구조 : 시작줄 → 헤더 여러 줄 → 빈 줄 → 본문
  - 위의 구조를 가지고 있지만, 결국 **텍스트 바이트열**임
  - 요청과 응답은 시작 줄만 다르고 나머지 뼈대는 같음
- HTTP 구조의 필요성 
  - TCP는 바이트 흐름만 주고, 경계를 알려주지 않음 
  - HTTP는 바이트열 안에 "어디까지가 무엇인지"를 읽어낼 규칙을 직접 정함
  - 서버는 이 규칙을 바탕으로 요청을 파싱함
#### 핵심 구조
- 요청 :
  ``` plain text
  GET /index.html HTTP/1.1\r\n    ← 시작 줄(요청 줄)
  Host: localhost:8080\r\n        ← 헤더 (이름: 값)
  Connection: keep-alive\r\n      ← 헤더
  \r\n                            ← 빈 줄 (헤더의 끝)
  (본문: GET은 보통 없음)             ← 본문
  ```
- 응답 :
  ``` plain text
  HTTP/1.1 200 OK\r\n             ← 시작 줄(상태 줄)
  Content-Type: text/html\r\n     ← 헤더
  Content-Length: 13\r\n          ← 헤더
  \r\n                            ← 빈 줄
  Hello, World!                   ← 본문 (13바이트)
  ```
#### 예시 : 파서가 하는 일
``` plain text
GET /a.html HTTP/1.1\r\nHost: x\r\n\r\n
```
- 첫 `\r\n`까지 잘라서 시작 줄을 얻음 → 공백으로 나눠 메서드 `GET`, 경로 `\a.html`, 버전 `HTTP/1.1`까지 얻음
- 다음 `\r\n`까지가 헤더 한 줄이고, `Host : x`를 얻을 수 있음 → `:`를 기준으로 이름과 값을 나눔
- 바로 뒤에 `\r\n`이 또 있으니 빈 줄임 → 헤더 끝

<br>
<br>

---

## 시작줄 : 메서드와 상태 코드
#### 메서드
- `GET`
  - 리소스를 가져온다
  - 본문 없음
  - 서버 상태 변경은 하지 않는 것이 규약 → 동일 요청이 반복되어도 문제가 없음 (같은 요청에 대해 결과도 같아야 함)
  ``` plain text
  GET /a.html HTTP/1.1\r\n  
  Host: x\r\n 
  \r\n
  ```
- `POST`
  - 서버에 데이터를 보내서 처리시킴
  - 본문이 보통 있음 (폼 데이터, JSON 등)
  - 서버 상태 변경을 할 수 있음 → 동일 요청이 반복되면 오류 발생 가능 (반복하면 두 번 처리될 수 있음)
  ``` plain text
  POST /login HTTP/1.1\r\n
  Host: x\r\n
  Content-Length: 11\r\n
  \r\n
  id=kim&pw=1
  ```
#### 상태 코드
  - 첫 자리 기준 분류
    ``` plain text
    2xx : 성공 
    3xx : 다른 곳으로 가라 (리다이레트)
    4xx : 클라이언트 요청이 잘못됨
    5xx : 서버 쪽 문제
    ```
  - 필요한 상태 코드 미리 보기
    - `404 Not Found` : 요청한 경로에 리소스가 없다.
    - `403 Forbidden` : 리소스가 존재하지만 접근을 허용하지 않는다.
    - `405 Method Not Allowed` : 아는 메서드이지만 이 경로에는 불허
    - `400 Bad Request` : 요청 줄 모양이 깨짐
    - `500 Internal Server Error` : 서버 쪽 처리 중 실패
    - `501 Not Implemented` : 서버가 아예 모르는 메서드
    - `502` / `504` : 프록시가 뒤쪽 서버와 통신 실패 / 시간 초과

---

## 핵심 헤더
#### Host
- 의미 : 이 요청이 어느 사이트(호스트) 앞으로 온 것인가?
- 필요성
  - TCP 연결은 IP와 포트까지만 정함.
  - 한 서버에 여러 사이트를 올릴 수 있음. 다 같은 소켓으로 받아짐
  - 서버는 시작줄만 봐서는 어느 사이트의 리소스를 요청하는 것인지 모름
  - `Host`를 통해 어떤 사이트의 리소스인지 구분 가능해짐
- 특징
  - HTTP/1.1에서는 요청에 반드시 포함되어야 함. 없으면 400 에러
#### Content-Type
- 의미 : 본문이 어떤 형식인가
  ``` plain text
  Content-Type: text/html          ← HTML 문서
  Content-Type: image/png          ← PNG 이미지
  Content-Type: application/json   ← JSON
  ```
- 필요성
  - 본문은 결국 바이트열일 뿐 → 받는 쪽이 이 값을 보고 해석 방법을 정함
  - 같은 바이트도 `text/html`이면 브라우저가 화면으로 그리고, `image/png`이면 이미지로 그림
  - 값의 모양은 MIME 타입 (`종류/세부`)임. 이게 하나의 형식.
  - 서버 입장에서는 응답할 때 직접 써야 하고, 요청에서는 POST 본문의 형식을 알려줄 때 읽음 
#### Content-Length
- 의미 : 본문이 몇 바이트인가
  - 단위는 글자가 아니라 바이트임 
  - 길이에는 본문만 셈. 시작줄, 헤더, 빈 줄은 포함하지 않음
#### Connection
- 의미 : 이 응답/요청 후 연결을 유지할지 닫을지
  ``` plain text
  Connection: keep-alive    ← 이 연결을 끊지 말고 다음 요청도 받자
  Connection: close         ← 이번 응답 후 이 연결을 닫자
  ```
  - 클라이언트가 유지/종료 희망을 요청에 작성 → 서버가 응답에 실제 결정을 적음
  - HTTP/1.1은 헤더를 안 적으면 유지가 기본, 닫고 싶을 때 close

---
## 경계 식별
``` mermaid
flowchart TD
    A["read 결과를 버퍼 뒤에 붙임"] --> B{"버퍼에 #92;r#92;n#92;r#92;n 있음?"}
    B -- 아니오 --> A
    B -- 예 --> C["헤더 파싱 후 Content-Length 확인"]
    C --> D{"Content-Length 있음?"}
    D -- 아니오 --> F["요청 완성: 본문 없음"]
    D -- 예 --> E{"헤더 뒤 바이트 수 >= Content-Length?"}
    E -- 아니오 --> A
    E -- 예 --> G["요청 완성: 본문 N바이트까지만 사용"]
    F --> H["남은 바이트는 다음 요청용으로 버퍼에 보관"]
    G --> H
```
- 메시지 끝을 찾는 일이 두 단계로 나뉨
  - `\r\n\r\n` : 헤더가 끝난 지점 찾기
  - Content-Length : 본문이 끝나는 지점 확인하기
- 예외 상황 : **chuncked**
  - 길이를 미리 알 수 없는 응답 (프로그램이 출력을 만들어 내는 동안 바로 내보내는 경우)
  - Content-Length 대신 조각마다 길이를 앞에 붙이고, 길이 0 조각으로 끝을 알림
- 응답에서 Content-Length나 chuncked 없이 본문이 있으면 "연결이 닫힐 때까지가 본문"으로 해석됨

--- 

## keep-alive
> 연결 하나로 여러 요청 처리하기
- **keep-alive** : 응답을 보낸 뒤 연결을 닫지 않고, 같은 TCP 연결로 다음 요청을 이어서 받는 방식
  - 연결마다 3-way handshake를 해야 하고, 연결을 닫을 때마다 TIME_WAIT이 남음 → 연결을 요청마다 새로 맺는 일은 낭비
  - 한 번 맺은 연결에 요청, 응답을 여러 번 실어서 이 비용을 줄이는 것이 목적
  - 메시지 경계가 정확해야만 성립함. Content-Length 또는 chunked가 필수.
- 시퀀스 다이어그램
  ``` mermaid
  sequenceDiagram
    participant C as 클라이언트
    participant S as 서버
    Note over C,S: TCP 연결 1회 (3-way handshake)
    C->>S: GET /a HTTP/1.1 (Connection: keep-alive)
    S-->>C: 200 OK (Content-Length: 5, Connection: keep-alive)
    C->>S: GET /b HTTP/1.1
    S-->>C: 200 OK (Content-Length: 8)
    C->>S: GET /c HTTP/1.1 (Connection: close)
    S-->>C: 200 OK (Connection: close)
    S->>C: FIN (서버가 close 호출)
    C->>S: FIN
  ```
- 서버 쪽 특징 : 연결 하나에 대해 {요청 읽기 → 응답 쓰기}를 반복하는 루프가 생김
  ``` plain text
  연결 열림
  반복:
      요청 하나를 완성할 때까지 read         
      응답 write
      이 연결을 유지할지 판단                
      유지하지 않으면 → close하고 반복 종료
  ```
  - 요청에 `Connection : close`가 있으면 응답 후 닫음
  - 클라이언트가 먼저 닫으면 `read()`가 0을 반환. 이 때 서버도 `close()`
  - 서버가 `close`를 정했다면 응답에도 `Connection : close`를 적어서 클라이언트에게 전달

<p align="right">
  <a href="./04-socket.md">⬅ 이전 페이지</a>
  &nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="./06-static-dynamic.md">다음 페이지 ➡</a>
</p>
