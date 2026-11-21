[⬅ overview](./00-overview.md)

---

# 3. TCP 기초
> - IP 주소와 포트, TCP와 UDP의 차이
> - 3-way handshake(연결), 4-way handshake(종료), TIME_WAIT
> - 바이트 스트림: TCP에는 메시지 경계가 없다

---

## IP 주소와 포트
- 둘을 합친 `IP:Port`가 통신 한 쪽 끝의 완전한 주소.
  - **IP 주소** : **어느 컴퓨터인가**
  - **포트** : 그 컴퓨터 안의 **어느 프로그램인가**
- **필요성** : IP 주소를 바탕으로 패킷이 컴퓨터에 도착. 도착 후 컴퓨터의 어느 프로그램에게 줘야 할지 구분하기 위해 포트 사용
- 핵심 구조
  | | IP 주소 | 포트 |
  | --- | --- | --- |
  | 가리키는 것 | 컴퓨터 (정확히는 네트워크 인터페이스) | 그 컴퓨터 안의 통신 끝점 (프로그램) |
  | 레이어 | 인터넷 층 (IP 헤더) | 전송 층 (TCP/UDP 헤더) |
  | 크기 | IPv4는 32 비트, 예 : `192.168.0.10` | 16 비트, 0~65535 |
  | 라우터 | 읽음 | 읽지 않음 |
- 포트 번호 범위
  - 0~1023 : well-known
  - 1024~49151 : 등록된 포트
  - 49152~65535 : 임시 포트 

#### 구체적 예시
> 브라우저로 `http://127.0.0.1:8000/`에 접속
``` plain text
브라우저(클라이언트)                         웹서버
  127.0.0.1 : 53412  ───── 요청 ─────▶  127.0.0.1 : 8080
  (53412는 OS가 임시로 배정)               (내가 정한 포트)
```
- **연결의 양쪽 모두 IP와 포트를 가짐**
- 서버 포트는 내가 정하고, 클라이언트 포트는 OS가 정함 (클라이언트도 bind로 직접 포트 선택 가능)
- 같은 서버에 브라우저 탭 두 개가 접속하면 다음과 같이 됨
  ``` plain text
  탭 A  127.0.0.1:53412 ──▶ 127.0.0.1:8080
  탭 B  127.0.0.1:53413 ──▶ 127.0.0.1:8080
  ```
- 포트가 하나라고 해서 연결이 하나인 것은 아님. 서버는 8080 하나로 수많은 클라이언트 연결을 받음.
- **IP는 컴퓨터, 포트는 프로그램**이라고 인식해도 되지만, 엄밀히는 포트가 프로그램에 1:1 대응 되는 것은 아니고, OS가 관리하는 소켓에 대응됨.

---

## TCP와 UDP의 차이
- **TCP** : 연결을 맺고, 빠짐없이 순서대로 바이트를 전달해주는 전송 방식
- **UDP** : 연결 없이 조각(데이터그램)을 그냥 던지는 방식
- 핵심 구조
  | | TCP | UDP |
  | --- | --- | --- |
  | 연결 | 먼저 연결을 맺음 (handshake) | 없음, 바로 보냄 |
  | 전달 보장 | 유실되면 재전송 | 보장 없음 |
  | 순서 | 보낸 순서대로 전달 | 뒤바뀔 수 있음 |
  | 데이터 단위 | 바이트 스트림(경계 없음) | 데이터그램(보낸 단위 그대로 전달) |
  | 소켓 타입 | `SOCK_STREAM` | `SOCK_DGRAM` |
  | 서버 호출 | `listen`, `accept` | 없음 (`recvfrom`으로 받음) |
  | 대표 용도 | HTTP, SSH, DB | DNS, 실시간 음성 및 영상, 게임 |
- 예시 : 클라이언트가 조각 1, 2, 3을 보냈는데 네트워크에서 2번이 사라진 상황
  ``` plain text
  UDP:  받는 쪽 앱이 1, 3을 받음. 2가 없다는 사실도 OS는 알려주지 않음.
      (필요하면 앱이 번호를 붙여 직접 감지·재요청해야 함)

  TCP:  받는 쪽 OS가 2가 비었음을 알아채고 재전송을 받음.
        앱은 1, 2, 3을 순서대로 받음. 앱은 유실이 있었는지도 모름.
  ```
  - 웹 서버는 순서, 조립, 재전송을 OS에게 맡길 수 있는 TCP를 씀
  - 약간의 데이터 유실이 있더라도 최신 데이터가 중요한 음성, 게임 등에서는 UDP를 씀
- TCP와 UDP는 포트 공간을 따로 씀. 프로토콜마다 포트 번호 공간이 따로 있으므로, TCP 8080과 UDP 8080은 별개임.

#### 메시지 경계 보존 
> 보낸 쪽이 한 번에 보낸 덩어리가 받는 쪽에서도 같은 덩어리 하나로 전달되는지
``` plain text
send("AAA")   send("BBBBB")   send("CC")
```
- `UDP` : 경계가 보존됨. recvfrom 한 번 호출할 때마다 정확히 한 덩어리를 받음
  - **이유** : 패킷 하나가 곧 메시지. 전송 층이 데이터를 따로 가공하지 않아서 데이터가 그대로 남음
  ``` plain text
  recvfrom() → "AAA"
  recvfrom() → "BBBBB"
  recvfrom() → "CC"
  ```
- `TCP` : 받는 쪽에서는 연속된 바이트의 흐름만 있음. 보내는 쪽의 send 횟수와 받는 쪽의 read 횟수가 일치한다는 보장 없음
  - **이유** : 신뢰성을 위해서 데이터를 OS가 자유롭게 쪼개고 합쳐서 내보냄 (네트워크 상태와 재전송 고려). 
  ``` plain text
  read() → "AAABB" (한 번에 일부만)
  read() → "BBBCC" (다음 호출은 나머지)
  또는
  read() → "AAABBBBBCC" (전부 한꺼번에)
  ```  

---

## 3-way handshake
- **3-way handshake란?**
  - TCP는 데이터를 보내기 전 양쪽이 메시지 3개(`SYN`→`SYN + ACK`→`ACK`)을 주고 받음
  - "서로 보내고 받을 수 있음"을 확인하는 과정
  - OS가 함
- **필요성** :
  - TCP는 순서를 맞추고 빠진 것을 재전송함
  - 이를 유지하기 위해서는 양쪽이 "상태"를 갖고 있어야 함
  - 어디까지 보냈는지, 어디까지 받았는지를 기억할 필요
  - 연결을 맺으면서 상태를 준비하고, 이 때 정하는 핵심이 **시퀀스 번호**
  - 보내는 바이트에 번호를 매겨서 받는 쪽이 순서를 맞추고 빠진 부분을 알 수 있게 함.
#### 핵심 구조
``` plain text
SYN (synchronize) : 연결 시작, 자신의 시작 번호 알림
ACK (acknowledge) : 상대방이 보낸 세그먼트 번호를 잘 받았다는 확인
ISN (Initial Sequence Number) : 각자 정한 번호
```
- 연결 상태
  - 서버 : `LISTEN` → `SYN_RECEIVED` → `ESTABLISHED`
  - 클라 : `SYN_SENT` → `ESTABLISHED`
- 보통 handshake에는 HTTP 요청이 실려 가지 않음. 연결을 맺은 뒤에 요청을 보냄
  - 요청 하나를 보내기 전에 왕복 한 번의 시간이 먼저 듬 (세 번째 ACK 보낼 때 메시지 함께 요청)
  - 매 요청마다 handshake하는 것의 부담 → keep-alive(연결 하나로 여러 요청)가 등장
#### 소켓 API와의 연결
``` plain text
서버:  listen()           → OS가 SYN을 받을 준비(LISTEN 상태)
클라:  connect()          → OS가 SYN을 보내고 서버의 SYN+ACK를 기다림. 성공하면 return
서버:  accept()           → handshake가 끝난 연결을 꺼내 옴
```
- 서버가 accept할 때 handshake를 시작하는 것이 아님.
  - handshake는 accept 호출 전에 이미 끝나 있을 수 있음 (listen 호출 이후, OS는 들어오는 SYN 요청들에 의해 handshake 진행
  - OS가 완료된 연결들을 대기열에 쌓아둠. (listen 대기열)
  - accept의 반환값은 새로운 fd임
#### socket의 구분 
> 서버는 소켓이 두 종류, 클라이언트는 연결당 소켓 하나
- 서버의 소켓
  ``` plain text
  listen_fd = socket(...); bind(...); listen(listen_fd, ...);

  conn_fd = accept(listen_fd, ...);   ← 대기열에서 연결 하나 꺼냄, 새 fd 반환
  read(conn_fd, buf, ...);            ← 그 클라이언트가 보낸 바이트 읽기
  write(conn_fd, resp, ...);          ← 그 클라이언트에게 응답
  ```
  - listen_fd : 대기용, 새 연결을 받는 입구. read/write 안 함. 서버가 켜져 있는 동안 계속 유지
  - conn_fd는 accept()가 호출될 때마다 새로 생김. 이 연결을 cloase()하면 사라짐 (listen_fd는 유지)
- 클라이언트의 소켓
  ``` plain text
  fd = socket(...)
  connect(fd, 서버주소)   ← 같은 fd가 그대로 연결된 소켓이 됨
  write(fd, ...); read(fd, ...)
  ```
  - 클라이언트는 socket()으로 만든 소켓 하나를 connect()로 연결하고 그대로 씀
  - 이 소켓은 OS가 임시 포트를 배정한 연결 하나에 대응함

``` mermaid
sequenceDiagram
    autonumber
    participant CA as 클라이언트 앱
    participant CO as 클라이언트 OS
    participant SO as 서버 OS
    participant SA as 서버 앱

    Note over SA,SO: 서버 준비 (앱이 호출)
    SA->>SO: socket() → listen_fd
    SA->>SO: bind(listen_fd, 8080)
    SA->>SO: listen(listen_fd)
    Note over SO: LISTEN 상태, 대기열 준비
    SA->>SO: accept(listen_fd) 호출
    Note over SA: 대기열이 비어 있으면 blocking

    Note over CA,CO: 클라이언트 연결 시도
    CA->>CO: socket() → fd
    CA->>CO: connect(fd, 서버IP:8080)
    Note over CA: connect()는 handshake가 끝날 때까지 blocking

    Note over CO,SO: 3-way handshake (OS끼리, 앱은 관여 안 함)
    CO->>SO: ① SYN (seq=x)
    Note over SO: SYN_RECEIVED (임시 기록)
    SO->>CO: ② SYN+ACK (seq=y, ack=x+1)
    Note over CO: ESTABLISHED
    CO->>SO: ③ ACK (ack=y+1)
    Note over SO: ESTABLISHED → 완료된 연결을 대기열에 넣음

    CO-->>CA: connect() return (성공)
    SO-->>SA: accept() return → conn_fd (새 fd)

    Note over CA,SA: 이후 데이터 교환
    CA->>CO: write(fd, "GET / ...")
    CO->>SO: 데이터 (4값으로 conn 소켓에 전달)
    SA->>SO: read(conn_fd)
    SO-->>SA: 받은 바이트
```
---

## 4-way handshake(종료)와 TIME_WAIT
- TCP 연결은 양방향이 각각 따로 닫히기 때문에 `FIN`과 `ACK`가 방향마다 한 쌍씩, 모두 4개의 메시지로 종료됨
- 왜 4 개인가?
  - `FIN` : "나는 더 보낼 것이 없다"는 뜻
  - 상대 앱은 아직 보낼 데이터가 남았을 수 있음 → 상대는 준비되면 자기 `FIN`을 따로 보냄
  - 시작할 때는 서버가 `SYN`과 `ACK`를 한 메시지에 합칠 수 있었지만, 종료에서는 `FIN`을 보내는 타이밍이 `ACK`와 같지 않음
#### 흐름
- 클라이언트가 먼저 `cloase()`하는 경우
  ``` mermaid
  sequenceDiagram
    autonumber
    participant CA as 클라이언트 앱
    participant CO as 클라이언트 OS
    participant SO as 서버 OS
    participant SA as 서버 앱

    Note over CO,SO: 둘 다 ESTABLISHED
    CA->>CO: close(fd)
    CO->>SO: ① FIN
    Note over CO: FIN_WAIT_1
    Note over SO: CLOSE_WAIT
    SO->>CO: ② ACK
    Note over CO: FIN_WAIT_2
    SO-->>SA: read(conn_fd) → 0 (상대가 보낼 것을 끝냄)
    Note over SA: 남은 응답을 write()할 수도 있음
    SA->>SO: close(conn_fd)
    SO->>CO: ③ FIN
    Note over SO: LAST_ACK
    CO->>SO: ④ ACK
    Note over SO: CLOSED
    Note over CO: TIME_WAIT (일정 시간 대기 후 CLOSED)
  ```
- 먼저 `close()`한 쪽이 마지막에 `TIME_WAIT`로 남음
- 상태가 `FIN`을 보내면 내 `read()`가 0을 반환함

#### TIME_WAIT
- `ACK`를 보낸 쪽은 곧바로 연결 정보를 지우지 않고 한동안 기다림
  - 마지막 ACK가 유실될 수 있음. 상대가 ACK를 받지 못하면, FIN을 다시 보냄. 연결 정보를 바로 지워버리면 이에 대답할 수 없음.
  - 이전 연결의 늦게 도착한 패킷이 있을 수 있음. FIN 도착이 이전에 보낸 패킷보다 빠를 수 있음.
- 서버를 종료한 뒤, 곧바로 재시작하면 bind가 남아 있어 `Address already in use`로 실패하는 일이 흔함
  - 서버가 먼저 연결을 닫아 TIME_WAIT가 남아 있기 때문
  - `SO_REUSEADDR`로 해결 (나중에 나옴)
- 클라이언트와 서버가 비슷한 시기에 close하는 경우
  ``` mermaid
  sequenceDiagram
      autonumber
      participant A as 호스트 A
      participant B as 호스트 B
  
      Note over A,B: 둘 다 ESTABLISHED
      A->>B: FIN
      B->>A: FIN
      Note over A: FIN_WAIT_1 상태에서 상대 FIN 수신 → CLOSING
      Note over B: FIN_WAIT_1 상태에서 상대 FIN 수신 → CLOSING
      A->>B: ACK
      B->>A: ACK
      Note over A,B: 각자 ACK 수신 → 둘 다 TIME_WAIT → 일정 시간 후 CLOSED
  ```

---

## 바이트 스트림 : TCP에는 메시지 경계가 없다
- TCP가 전달하는 것은 "메시지들"이 아니라 순서가 보장된 바이트의 연속된 흐름
- 요청 하나가 어디서 끝나는지는 TCP가 알려주지 않고 내 코드가 판단해야 한다
- 이유
  - TCP는 send 단위를 받는 쪽에 전달하지 않음
  - 한 패킷에 실을 수 있는 한계가 있고, 큰 데이터는 여러 조각으로 쪼개짐
  - 작은 write를 여러 번 하면 OS가 모아서 한 번에 보내기도 함
- **TCP 입장에서는 "바이트 순서만 맞으면 정상"임**
#### 서버 코드에서의 파싱
``` mermaid
sequenceDiagram
    participant C as 클라이언트 앱
    participant SO as 서버 OS (수신 버퍼)
    participant SA as 서버 앱

    C->>SO: "GET /index.html HTT" (첫 조각 도착)
    SA->>SO: read(conn_fd)
    SO-->>SA: "GET /index.html HTT" (19바이트)
    Note over SA: 요청 끝(#92;r#92;n#92;r#92;n)이 없음 → 아직 미완성, 버퍼에 보관
    C->>SO: "P/1.1#92;r#92;nHost: x#92;r#92;n#92;r#92;n" (나머지 도착)
    SA->>SO: read(conn_fd)
    SO-->>SA: "P/1.1#92;r#92;nHost: x#92;r#92;n#92;r#92;n"
    Note over SA: 앞 조각과 이어 붙임 → #92;r#92;n#92;r#92;n 발견 → 요청 1개 완성
```
- read()의 반환값을 바로 요청으로 처리하면 안 됨
- 서버는 읽은 바이트를 **자기 버퍼에 이어 붙이면서** "요청이 끝났다는 표시"가 보일 때까지 read()를 반복해야 함
  ``` plain text
  buf = 빈 버퍼
  loop:
      n = read(conn_fd, tmp)
      if n == 0: 상대가 FIN을 보냄 → 종료 처리 (3-4)
      buf 뒤에 tmp를 붙임
      if buf에 "\r\n\r\n"이 있다: 요청 헤더 완성 → 파싱
      else: 계속 read
  ```
- TCP는 끝을 알려주지 않고, 그 판단은 응용 프로토콜(HTTP)의 규칙을 써서 내 코드가 해야 함
- 요청이 합쳐져 오는 경우
  ``` plain text
  read() → "GET /a HTTP/1.1\r\n...\r\n\r\nGET /b HTTP/1.1\r\n...\r\n\r\n"
  ```
  - read로 한 번에 두 개 이상의 요청이 들어올 경우
  - 서버는 첫 요청까지만 처리하고, 남은 바이트는 버퍼에 남겨웠다가 다음 요청의 시작으로 써야 함


<br>
<br>

---

<p align="right">
  <a href="./02-network-layers.md">⬅ 이전 페이지</a>
  &nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="./04-socket.md">다음 페이지 ➡</a>
</p>
