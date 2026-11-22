[⬅ overview](./00-overview.md)

---

# 4. 소켓 프로그래밍 (과제와 직결)
> - 파일 디스크립터: 소켓도 파일처럼 다룬다
> - socket, bind, listen, accept, connect, read, write, close
> - blocking 동작, listen 대기열, SO_REUSEADDR, bind 실패 원인
> - read/write가 요청한 만큼 다 처리하지 않는 경우(short read/write)

---

## 파일 디스크립터
> 소켓도 파일처럼 다룬다.
- **파일 디스크립터(fd)** : 프로세스가 커널 안의 "열린 입출력 대상"을 가리킬 때 쓰는 작은 정수 번호
  - 대상이 파일이든, 터미널이든, 소켓이든, 같은 종류의 번호표로 다룸 → `read`/`write`/`close`를 일률적으로 적용 가능
  - 이유 : 입출력 대상은 전부 "바이트가 드나드는 통로로 본다"는 설계 철학 때문.
    - 대상마다 읽기 함수가 따로 있으면 `read_file`, `read_socket`, `read_terminal`처럼 함수가 늘어남
    - 하나로 통일한 인터페이스 → 하나의 `read(fd, ...)` 함수로 처리 가능
#### 핵심 구조
``` mermaid
flowchart LR
  subgraph P["내 프로세스: fd 테이블"]
    f0["0"]
    f1["1"]
    f2["2"]
    f3["3"]
    f4["4"]
    f5["5"]
  end
  subgraph K["커널: 열린 대상"]
    t["터미널 입력"]
    o["터미널 출력"]
    e["터미널 에러 출력"]
    file["a.txt 파일"]
    ls["대기 소켓 :8080"]
    cs["연결 소켓: 클라이언트 A"]
  end
  f0 --> t
  f1 --> o
  f2 --> e
  f3 --> file
  f4 --> ls
  f5 --> cs
```
- fd는 0 이상의 정수.
- fd가 가리키는 실제 대상은 커널이 가지고 있고, 프로세스들은 fd만 알고 있음
  - 프로세스가 실제 읽기와 쓰기를 할 때는 내부 객체를 직접 건드리지 않고 시스템 콜을 통해서만 진행됨
- 0, 1, 2는 프로세스가 시작될 때 보통 이미 열려 있음 (stdin, stdout, stderr)
- 새 fd를 만드는 호출(`open`, `socket`, `accept`)은 지금 비어 있는 가장 작은 번호를 줌
- 주의 사항
  - fd 번호는 프로세스 안에서만 의미가 있음
  - fd는 유한한 자원임 (프로세스별 한도) → `close`로 자원 회수 필요
  - `close(fd)`는 번호표를 반납하는 것임. 커널 객체가 바로 사라지는 것이 아님
#### 같은 file을 커널 내부에서 처리하는 방법
- 커널 안에서는 대상마다 다른 코드가 실행됨
- 분기는 커널 안에서 일어나고, fd가 가리키는 함수 포인터 표를 바탕으로 서로 다른 `read`를 처리하는 방식
  ``` mermaid
  sequenceDiagram
    participant App as 내 코드
    participant Sys as 커널 read 진입점
    participant Obj as fd가 가리키는 객체
    participant Impl as 대상별 구현
  
    App->>Sys: read(fd, buf, n)
    Sys->>Sys: fd 테이블에서 fd 번호로 객체 찾기
    Sys->>Obj: 이 객체의 read 함수를 호출
    alt 일반 파일
      Obj->>Impl: 파일시스템 read, 디스크나 페이지 캐시에서 읽기
    else 소켓
      Obj->>Impl: 소켓 read, TCP 수신 버퍼에서 꺼내기
    else 터미널
      Obj->>Impl: 터미널 드라이버 read, 키보드 입력 받기
    end
    Impl-->>App: 읽은 바이트 수
  ```
- **VFS (Virtual File System)** : 전체 계층을 file로 추상화 함
- **f_op** : 함수 표를 따라서 구현이 갈림. 다형성(polymorphism)의 C식 표현
- **private_data** : 불투명 포인터, 컨텍스트 포인터라고도 함. 대상별 데이터를 달아두는 방식
    ``` mermaid
    flowchart LR
    fd3["fd 3"] --> F1
    fd5["fd 5"] --> F2
  
    subgraph F1["file 객체 ①: 일반 파일 a.txt"]
      direction TB
      a1["f_op"]
      a2["private_data: 보통 비어 있음"]
    end
  
    subgraph F2["file 객체 ②: TCP 연결 소켓"]
      direction TB
      b1["f_op"]
      b2["private_data"]
    end
  
    a1 --> T1["파일시스템용 함수 표<br/>read, write, release"]
    b1 --> T2["소켓용 함수 표<br/>read, write, release"]
    b2 --> S["소켓 객체 → sock → tcp_sock<br/>수신 버퍼, 시퀀스 번호 등"]
  
    classDef shell fill:#EEEDFE,stroke:#534AB7,color:#26215C
    classDef table fill:#E1F5EE,stroke:#0F6E56,color:#04342C
    classDef data fill:#FAEEDA,stroke:#854F0B,color:#412402
    class a1,a2,b1,b2 shell
    class T1,T2 table
    class S data
    style F1 fill:#F7F6FF,stroke:#534AB7
    style F2 fill:#F7F6FF,stroke:#534AB7
    ```

---

## 서버 쪽 호출 흐름
> `socket` → `bind` → `listen` → `accept`

``` mermaid
sequenceDiagram
  participant S as 서버 코드
  participant K as 서버 OS
  participant C as 클라이언트

  S->>K: socket(AF_INET, SOCK_STREAM, 0)
  K-->>S: listen_fd = 3
  S->>K: bind(3, 포트 8080)
  K-->>S: 성공
  S->>K: listen(3, backlog)
  K-->>S: 성공, 즉시 return
  Note over K: 이제부터 SYN을 받으면 OS가 handshake 처리
  S->>K: accept(3)
  Note over S: 완성된 연결이 없으면 여기서 멈춤
  C->>K: SYN
  K-->>C: SYN+ACK
  C->>K: ACK
  Note over K: handshake 완료, 연결이 대기열에 들어감
  K-->>S: conn_fd = 4 (accept가 return)
  Note over S: 이제 4번 fd로 read와 write
```
| 호출 | 하는 일 | 앞에서 배운 것과의 연결 |
| --- | --- | --- |
| socket | 커널 안에 통신 끝점 객체를 만들고 fd를 돌려줌 | 번호표(fd)와 커널 객체. 이 시점엔 주소도 없고 아무것도 기다리지 않음 |
| bind | 이 소켓에 IP와 포트를 붙임 | 서버 포트는 클라이언트가 먼저 찾아와야 하므로 고정. 과제에서는 CLI 인자(assignment.md 74행) |
| listen | 이 소켓을 "접속을 받는 대기 소켓"으로 전환 | 이 순간부터 OS가 클라이언트의 SYN을 받아 handshake를 처리 | 
| accept | handshake가 끝난 연결 하나를 꺼내 새 fd를 돌려줌 | 연결 소켓(conn_fd)은 클라이언트마다 새로 생김 |

#### socket
- `socekt()` 
  - 커널 안에 소켓 객체를 만들고, 그 번호표(fd)를 돌려줌
  - 만든 직후의 소켓은 주소도 없고, 연결도 없고, 아무도 기다리지 않는 빈 상태
    - 대기 상태가 아니기 때문에 접속 불가
    - read/write를 해도 읽고 쓸 상대가 없음 (연결 없음)
  - `file` 객체의 `f_op`를 소켓용 함수 표로 채워 넣음
  - 이후 `bind`, `listen`, `accept`가 어떤 소켓에 대한 요청인지 가리킬 번호를 받음
- 호출 모양
  ``` c
  int socket(int domain, int type, int protocol);
  ```
  - `domain` : 주소 체계. 어떤 형식의 주소로 통신하는 가. 예: `AF_INET`(IPv4주소 + 포트)
  - `type` : 통신 방식. TCP/UDP 구분. 예 : `SOCK_STREAM`
  - `protocol` : 위 둘로 정해지는 후보 중 구체적인 프로토콜. 0이 기본값
  - 반환값 : fd (0 이상의 정수). 실패 시 `-1`
#### bind
- `bind()`
  - 생성한 빈 소켓에 **로컬 주소(IP + 포트)**를 붙임
  - 서버 포트는 클라이언트가 먼저 찾아올 수 있도록 서버 프로그래머가 정해둠
  - 이 binding을 통해 클라이언트가 "이 컴퓨터의 8080번"이라는 목적지로 찾아옴
  - `socket()`까지만 한 소켓은 주소가 없어서 커널이 도착한 패킷을 "이 소켓의 것"으로 연결할 기준이 없음
- 호출 모양
  ``` c
  int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
  ```
  - `sockfd` : 주소를 붙일 소켓의 fd
  - `addr` : 붙일 주소. IPv4면 `struct sockaddr_in`을 채워서 넘김 
    ``` c
    struct sockaddr_in addr = {0};              // 먼저 전부 0으로 초기화
    addr.sin_family      = AF_INET;             // socket()의 domain과 일치해야 함
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // IP: 이 컴퓨터의 모든 IP
    addr.sin_port        = htons(8080);         // 포트: CLI 인자에서 온 값이 들어갈 자리
    
    bind(listen_fd, (struct sockaddr *)&addr, sizeof addr);
    ```
  - `addrlen` : addr에 넘긴 구조체의 크기
  - 반환값 : 성공 0, 실패 -1
- 포트 문제
  - 비어 있는 포트를 잘 선택 → 성공
  - 다른 프로세스가 사용 중인 포트로 시도 → 실패 (`EADDRINUSE`)
  - 1024 미만 포트 일반 사용자 권한으로 사용 → 실패 (`EACCES`)
  - TIME_WAIT 연결이 아직 남아 있음 → 실패 (`EADDRINUSE` 또는 `SO_REUSEADDR`)
#### listen
- `listen()`
  - `bind`된 소켓을 "접속 받는 대기 소켓"으로 전환
  - 이 순간부터 OS가 클라이언트의 SYN을 밭아 handshake를 대신 처리 & 완성된 연결을 대기열에 쌓아둠
  - 호출 자체는 기다리지 않고 바로 return
  - 주로 TCP 같은 연결형 소켓(`SOCK_STREAM`)용 시스템 콜
- 호출 모양
  ``` c
  int listen(int sockfd, int backlog);
  ```
  - `sockfd` : 대기 소켓으로 바꿀 fd (bind까지 끝낸 상태여야 함)
  - `backlog` : 아직 `accept`되지 않고 대기열에 쌓아둘 수 있는 연결 수의 힌트
- listen 과정 (전후) 시퀀스
  ``` mermaid
  sequenceDiagram
    participant S as 서버 코드
    participant K as 서버 OS
    participant C as 클라이언트
  
    S->>K: listen(listen_fd, 128)
    K-->>S: 0 (즉시 return)
    Note over K: listen_fd가 대기 소켓이 됨
    C->>K: SYN
    K-->>C: SYN+ACK
    C->>K: ACK
    Note over K: handshake 완료, 연결이 대기열에 보관됨
    Note over S: 서버 코드는 아직 accept를 안 불렀어도 상관없음
    S->>K: accept(listen_fd)
    K-->>S: 대기열에서 꺼낸 conn_fd
  ```
#### accept
- `accept()`
  - `listen` 소켓의 대기열에서 handshake가 끝난 연결 하나를 꺼냄
  - 꺼낸 연결 전용의 새 소켓과 새 fd(`conn_fd`)를 돌려줌 (연결 전용 fd가 생겨 `read`/`write` 호출 가능)
  - 대기열이 비어 있으면 연결이 올 때까지 멈춤 (blocking)
  - `listen`까지의 동작은 OS가 연결을 먼저 받아 두게 할 뿐, 그 연결을 서버 코드가 쓸 수 있는 fd로 건네주지는 않음
- 호출 모양
  ``` c
  int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
  ```
  - `sockfd` : 대기 소켓. 
  - `addr` : 접속한 클라이언트의 주소를 받아 올 버퍼. 필요 없으면 NULL
  - `addrlen` : addr 버퍼의 크기를 입력해 넣고, 실제 주소 크기를 받아옴. addr이 널이면 NULL
  - 반환값 : 성공하면 새 fd(`conn_fd`), 실패하면 -1
- 시간 순서 : 대기열 → conn_fd
  ``` mermaid
  sequenceDiagram
    participant S as 서버 코드
    participant K as 서버 OS
    participant Q as 완성된 연결 대기열
    participant C as 클라이언트
  
    S->>K: accept(listen_fd)
    Note over S: 대기열이 비어 있으면 여기서 멈춤
    C->>K: SYN, SYN+ACK, ACK (handshake)
    K->>Q: 완성된 연결 A 추가
    Q-->>K: 연결 A를 꺼냄
    K-->>S: conn_fd = 4, 클라이언트 주소 채움
    Note over S: listen_fd는 그대로 남음, 4번으로 read와 write
    S->>K: accept(listen_fd)
    Note over S: 다음 연결을 기다림 (대기열에 이미 있으면 바로 return)
  ```

---

## 클라이언트 쪽 호출 흐름
> `connect` → `read`/`write`/`close`

#### connect
- `connect()`
  - 클라이언트 소켓을 지정한 서버 주소 (IP + 포트)에 연결
  - 내부적으로 handshake를 시작하고, 끝나면 return
  - 서버는 기다리는 쪽이고, 연결은 클라이언트가 먼저 시작.
  - 프록시는 클라이언트에게는 서버이지만 백엔드에게는 클라이언트이기 때문에, 백엔드로 나가는 연결을 connect로 만듦
  - 클라이언트는 bind하지 않더라도, 생성한 소켓으로 connect를 하면 OS가 임시 포트를 붙여줌
- 호출 모양
  ``` c
  int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
  ```
  - bind와 모양이 거의 같지만, 의미가 반대임
  - 넘기는 주소는 상대 주소(서버)임. (bind에서는 자신 컴퓨터 주소)
    - `htonl(INADDR_ANY)` 대신 `inet_pton()`으로 특정 IP를 넣음
    - 도메인 이름은 IP로 바꾸는 별도 과정(`getaddrinfo`)이 필요함
  - 네트워크 통신이 필요한 작업임 (handshake)
  - 반환값 : 성공 0, 실패 -1
- 시간 순서
  ``` mermaid
  sequenceDiagram
    participant C as 클라이언트 코드
    participant CO as 클라이언트 OS
    participant SO as 서버 OS
  
    C->>CO: socket()
    CO-->>C: fd = 3
    C->>CO: connect(3, 서버IP:8080)
    Note over CO: 임시 포트 자동 배정, 예: 50321
    CO->>SO: SYN
    SO-->>CO: SYN+ACK
    CO->>SO: ACK
    CO-->>C: 0 (connect 성공)
    Note over C: 이제 3번 fd로 read와 write
  ```
- 실패 경우
  - 서버가 listen을 안 하거나 그 포트에 아무도 없음 → `ECONNREFUSED`
  - 서버 컴퓨터에 닿지 못하거나 SYN이 버려짐 → 한참 멈췄다가 시간 초과 (`ETIMEDOUT`)
#### read
- `read()`
  - fd의 수신 버퍼에서 최대 n바이트를 꺼내 내 버퍼에 복사
  - 반환값은 실제로 수신 버퍼에서 읽어온 바이트 수.
  - 버퍼가 비어 있으면 데이터가 도착할 때까지 멈춤 (blocking)
- 호출 모양
  ``` c
  ssize_t read(int fd, void *buf, size_t count);
  ```
  - `fd` : 읽을 fd (`conn_fd` 등 연결 소켓)
  - `buf` : 읽은 바이트를 받을 내 버퍼
  - `count` : 최대 몇 바이트까지 받을 수 있는지 (버퍼 크기 이하여야 함). count만큼 다 채워주지 않고, 1바이트라도 있으면 그만큼 돌려줌
  - 반환값 : 양수일 경우 읽은 바이트 수, 0일 경우 상대가 FIN을 보냈다는 표시. -1일 경우 오류
- 시간 순서
  ``` mermaid
  sequenceDiagram
    participant S as 서버 코드
    participant K as 서버 OS
    participant C as 클라이언트
  
    S->>K: read(conn_fd, buf, 4096)
    Note over S: 수신 버퍼가 비어 있어 여기서 멈춤
    C->>K: 요청 바이트 일부 도착
    K-->>S: 도착한 만큼 반환 (예: 60)
    S->>K: read(conn_fd, buf, 4096)
    Note over S: 다시 비었으면 또 멈춤
    C->>K: FIN
    K-->>S: 0 반환
    Note over S: 상대가 보내기를 끝냄
  ```
 #### write
 - `write()`
   - 내 버퍼의 바이트를 소켓의 송신 버퍼에 넣음
   - 반환값은 실제로 넣은 바이트 수를 돌려줌
   - 응답을 만든 뒤, 상대에게 데이터를 내부내는 유일한 통로임.
   - 내 코드는 응용 수준의 데이터(HTTP 문장 등)만 넘기고, TCP 헤더 부착, 분할, 재전송은 OS가 함
 - 호출 모양
   ```c
   ssize_t write(int fd, const void *buf, size_t count);
   ```
  - `fd` : 보낼 fd (`conn_fd` 등 연결 소켓)
  - `buf` : 보낼 바이트가 든 내 버퍼
  - `count` : 보내려는 바이트 수
  - 반환값 : 양수일 경우 송신 버퍼에 넣은 바이트 수 (count보다 작을 수 있음), -1일 경우 오류
- 시간 순서
  ``` mermaid
  sequenceDiagram
    participant S as 서버 코드
    participant K as 서버 OS
    participant C as 클라이언트
  
    S->>K: write(conn_fd, 응답, 200)
    Note over K: 송신 버퍼에 복사
    K-->>S: 200 반환 (OS가 맡음)
    Note over S: 이 시점에 클라이언트가 받았다는 보장은 없음
    K->>C: TCP 세그먼트 전송
    C-->>K: ACK
    Note over K: ACK가 와야 전달 확인, 안 오면 OS가 재전송
  ```
#### close
- `close(fd)`
  - fd 번호표를 반납하고, 그 fd가 가리키던 소캣을 가리키는 fd가 더 없으면 커널이 연결을 종료
  - 그 번호는 다음 `accept`나 `open`이 재사용
  - fd의 누수를 막으려면 연결이 끝날 때마다 연결 소켓의 fd를 close해야 함
  - `close`는 상대에게 나는 더 안 보낸다는 사실을 알리는 수단. 상대는 0을 받음.
  - 양방향을 모두 포기하는 선언임. 보내기만 닫고 응답을 더 읽으려면 shutdown을 써야 함.
- 호출 모양
  ``` c
  int close(int fd);   // 성공 0, 실패 -1 (+ errno)
  ```
- 시간 순서
  ``` mermaid
  sequenceDiagram
    participant S as 서버 코드
    participant K as 서버 OS
    participant C as 클라이언트

    S->>K: write(conn_fd, 응답)
    S->>K: close(conn_fd)
    Note over K: fd 반납, 송신 버퍼에 남은 데이터는 보통 계속 전송
    K->>C: 응답 데이터
    K->>C: FIN
    C-->>K: ACK
    Note over C: read가 응답을 받은 뒤 0을 반환
    C->>K: FIN (클라이언트가 close할 때)
    K-->>C: ACK
    Note over K: 먼저 FIN을 보낸 서버 쪽에 TIME_WAIT
  ```

---

## blocking 동작과 listen 대기열
#### blocking
- 조건이 충족될 때까지 호출한 프로세스(스레드)를 재워두고, 조건이 되면 깨워서 return하는 방식
- blocking 호출
  - `accept` : 완성된 연결 대기열이 비어있을 경우, 연결이 하나라도 완성될 때까지 block
  - `read` : 수신 버퍼가 비어 있을 경우, 1바이트 이상 혹은 FIN이 도착할 때까지 block
  - `write` : 송싱 버퍼에 자리가 없을 경우, 자리가 생길 때까지 block
  - `connect` : handshake 진행이 완료(실패)할 때까지 block
- "멈춘다"의 정확한 의미
  - 멈춘 프로세스는 CPU를 쓰면서 반복 확인(busy-waiting)하는 게 아니라, 커널이 **대기 상태**로 바꿔 스케쥴링에서 제외됨
  - block된 프로세스(스레드)들이 몇 시간을 기다려도, 이 프로세스들의 CPU 사용량은 거의 0에 가까움
  ``` mermaid
  sequenceDiagram
    participant S as 서버 프로세스
    participant K as 커널

    S->>K: accept(listen_fd)
    Note over K: 대기열이 비어 있음, 프로세스를 대기 상태로 전환
    Note over S: 실행 안 됨 (CPU 사용 없음)
    Note over K: 클라이언트 handshake 완료, 대기열에 연결 추가
    K->>S: 프로세스를 깨움
    K-->>S: conn_fd 반환
  ```
- 문제점 : **전체 작업이 멈춘다**
  ``` c
  while (1) {
      int c = accept(listen_fd, NULL, NULL);   // ①
      read(c, buf, sizeof buf);                // ② 클라이언트 A가 아무것도 안 보내면 여기서 무한정 멈춤
      write(c, resp, len);
      close(c);
  }
  ```
  - 클라이언트 A가 연결만 하고 요청을 보내지 않으면 서버는 ②에 멈춰서 ①로 돌아가지 못함
  - 다른 클라이언트가 접속하면, OS가 handshake를 해서 대기열에 넣지만, 서버 코드는 accept를 호출하지 못함
  - 이 문제를 해결하기 위해 **동시성 모델**이 필요해짐
#### listen 대기열과 `backlog`
- 3-way-handshake에서 사용하는 큐
  - SYN 큐 (미완성) : SYN은 받았고, ACK를 기다리는 중 → 나중에 ACK가 오면 완성 큐로 이동 
  - accept 큐 (완성) : handshake 완료, 서버의 `accept`를 기다리는 중 → 서버가 `accept`로 꺼냄 
- 여기서 말하는 listen 대기열은 **accept 큐**를 의미함
  - `listen`이 시작되면 OS는 아직 `accept`되지 않은 연결을 대기열에 쌓아둠
- 이 대기열의 크기에는 한도가 있음 → `backlog`가 그 한도를 정하는 힌트가 됨
  - 한도를 넘으면 새 연결 시도는 느려지거나 실패함 
  ``` mermaid
  sequenceDiagram
    participant C as 클라이언트
    participant K as 서버 OS
    participant Q as accept 큐 (가득 참)

    C->>K: SYN
    K-->>C: SYN+ACK
    C->>K: ACK
    Note over K,Q: 큐에 자리가 없음
    Note over C: connect는 성공했다고 느낄 수 있음
    Note over K: OS가 이 연결을 버림(또는 ACK를 무시)
    C->>K: 요청 데이터 write
    Note over C: 응답 없음, 재전송 반복 후 지연 또는 실패
  ```
- `backlog` : accept 큐에 쌓을 수 있는 힌트
  - OS가 상한을 따로 정해서, 큰 값을 줘도 잘릴 수 있음 (정확한 상한은 OS마다 다름)
  - OS가 상한을 정하지만 서버 앱이 backlog를 넘겨 양쪽이 나눠 정함
    - 앱 : **트래픽 규모**를 앎
    - OS : **실제 자원**을 쥐고 있음
  - 값이 작으면 순간적인 접속 몰림에 연결이 버려지기 쉽고, 크면 오래 쌓인 연결이 늘어남.
  - 본질적인 해결법은 `accept`를 빨리 부르는 것. (동시성 모델과 연결됨)

---

## `SO_REUSEADDR`과 bind 실패
#### bind 실패 원인과 TIME_WAIT
- 서버를 켰다가 바로 다시 켜면, 기본 설정에서는 `bind`가 `Address already in use`(`EADDRINUSE`)로 실패할 수 있음
- 서버 프로세스는 죽었지만, 커널에 남아 있는 연결 상태(TIME_WAIT 등)가 그 포트를 쥐고 있기 때문
  ``` mermaid
  sequenceDiagram
    participant S as 서버 프로세스
    participant K as 서버 OS
    participant C as 클라이언트

    S->>K: close(conn_fd), 서버가 먼저 닫음
    K->>C: FIN
    C-->>K: ACK
    C->>K: FIN
    K-->>C: ACK
    Note over K: 이 연결은 TIME_WAIT, 로컬 포트 8080을 쓰는 상태로 남음
    S->>K: 서버 종료 (listen_fd도 닫힘)
    Note over S: 서버를 다시 실행
    S->>K: bind(listen_fd, 8080)
    K-->>S: -1, EADDRINUSE (기본 설정일 때)
  ```
  - TCP 상태는 **소켓 fd/프로세스와 수명이 분리**되어 커널이 따로 들고 있음
  - 프로세스가 죽어도 TIME_WAIT은 남고, 그 상태가 로컬 주소 `(IP, 8080)`을 점유한 것으로 취급됨.
#### `setsokopt`와 `SO_REUSEADDR`
- `SO_REUSEADD` : `bind` 전에 소켓에 켜두는 옵션. 
  - `TIME_WAIT` 등 옛 연결 흔적이 남아 있어도 같은 포트에 `bind` 가능
  - 이미 `listen` 중인 다른 소켓과의 충돌은 풀어주지 않음
- 호출 모양
  ``` c
  int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen);
  ```
  - `sockfd` : 옵션을 줄 소켓 (`listen_fd`)
  - `level` : 옵션이 속한 층 (`SOL_SOCKET` - 소켓 일반 옵션)
  - `optname` : 옵션 이름 (`SO_REUSEADDR`)
  - `optval`, `optlen` : 옵션 값과 그 크기 (`int 1` - 켜기)
- 호출 위치 
  ``` c
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);  // bind 전에!

  bind(listen_fd, (struct sockaddr *)&addr, sizeof addr);
  listen(listen_fd, 128);
  ```
  - `bind` 뒤면 이미 실패한 뒤라 소용이 없음 
  - 설정하려는 옵션이 `bind`의 검사 기준을 바꾸는 것이기 때문

---

## short read / short write
#### short read
- **short read** : `read()` 의 반환값이 `count`보다 작은 경우 (정상 동작)
  - 오류는 아니므로, 앱은 반환값 만큼만 유효하다고 보고 필요한 만큼 `read`를 반복해야 함
- 원인
  - 상대 앱이 잘게 보내거나, TCP가 쪼개 전달함
  - `read` 시점에 수신 버퍼에 그만큼만 쌓여 있음
  - 내 `buf`/`count`가 작음
  - 시그널에 깨어나 일부만 읽고 반환, 또는 -1 반환
- 처리
  - 읽어야 할 길이를 정확히 아는 경우 (예 : `Content-Length : 100`)
    ``` c
    // 정확히 total 바이트를 채울 때까지 반복
    size_t got = 0;
    while (got < total) {
        ssize_t n = read(fd, buf + got, total - got);  // 이미 읽은 만큼 건너뜀
        if (n > 0)  got += n;
        else if (n == 0) { /* 상대가 중간에 닫음: 미완성, 오류 처리 */ break; }
        else if (errno == EINTR) continue;             // 시그널 중단은 재시도
        else { /* 진짜 오류 */ break; }
    }
    ```
  - 길이를 모르는 경우 (예 : HTTP 요청의 `\r\n\r\n`)
    ``` c
    // 끝 표시가 나올 때까지 입력 버퍼에 이어 붙임
    while (!found_end_marker(buf, len)) {
        ssize_t n = read(fd, buf + len, sizeof buf - len);
        if (n <= 0) break;       // 0 또는 오류 처리는 위와 동일
        len += n;
    }
    ```
#### short write
- **short write** : `write()`의 반환값이 `count`보다 작은 경우 (정상 동작)
  - 서버는 남은 부분만 다시 `write`하는 반복문으로 응답 전체를 보내야 함
- 원인 
  - 상대의 버퍼가 거의 찼을 때, 송신이 느려짐. 송신 버퍼에 자리가 일부만 있으면 일부만 쓰고 반환.
  - 시그널에 중단되는 경우 일부만 넣고 반환함.
  - non-blocking 소켓인 경우, 자리가 없으면 멈추지 않고 일부만 씀
- 오류
  - `EINTR` : 재시도 함. 오류가 아니라 중단임
  - `EPIPE` / `SIGPIPE` : 상대가 이미 연결을 닫았는데 `write`하면 `EPIPE`가 나고, `SIGPIPE` 시그널이 프로세스 전체를 종료시킴.

<br>
<br>

---

<p align="right">
  <a href="./03-tcp.md">⬅ 이전 페이지</a>
  &nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="./05-http.md">다음 페이지 ➡</a>
</p>
