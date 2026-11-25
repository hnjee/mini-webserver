[⬅ overview](./00-overview.md)

---

# 7. 동시성 모델
> - 왜 필요한가: 한 연결이 오래 붙잡으면 생기는 일
> - 멀티프로세스(fork), 스레드 풀(pthread), 이벤트 루프(non-blocking + select/poll/epoll)
> - 각 방식의 트레이드오프와 C10K 문제
> - epoll(Linux)과 kqueue(macOS)의 차이, 이식성 처리

---

## 동시성 문제
- **단순한 서버** : 연결을 하나씩 순서대로 처리
  ``` c
  while (1) {
      conn_fd = accept(listen_fd, ...);   // 대기열에서 연결 하나를 꺼냄
      handle(conn_fd);                    // read → 응답 만들기 → write
      close(conn_fd);
  }
  ```
  - 앞 연결이 `read`나 CGI 대기에서 멈추면 뒤 연결이 **전부 기다림**
  - 대기 지점
    - 느린 클라이언트의 read : 클라이언트가 연결만 맺고 요청을 보내지 않거나 조금씩 보냄
    - 느린 처리 : CGI가 오래 걸림
    - keep-alive 유휴 연결 : 클라이언트가 다음 요청을 안 보냄
- **동시성 모델** : 이 문제를 푸는 방법
  - 연결마다 프로세스를 따로 둔다 (멀티 프로세스)
  - 연결마다 스레드를 따로 둔다 (스레드 풀)
  - 한 프로세스가 여러 연결을 번갈아 본다 (이벤트 루프)

---

## 멀티 프로세스
- 연결 하나를 받을 때마다 fork로 자식 프로세스를 만들어 그 연결을 맡기고, 부모는 바로 accept로 돌아감
- 동시성 문제 해결 방향 
  - 한 흐름이 `accept`와 `handle`을 둘 다 맡은 데 있는 문제 → 둘로 나눔
  - 부모 : accept만 반복
  - 자식 : 연결 하나의 handle을 맡음. 자식이 A의 read에서 멈춰도 부모는 영향이 없음
- 구조
  ``` c
  while (1) {
      conn_fd = accept(listen_fd, ...);
      pid = fork();
      if (pid == 0) {              // 자식
          close(listen_fd);        // 자식은 새 연결을 받지 않음
          handle(conn_fd);
          close(conn_fd);
          _exit(0);
      }
      close(conn_fd);              // 부모: 이 연결은 자식 몫
  }
  ```
  - fork는 호출한 프로세스를 복제. 
  - 갈린 반환값에 따라 처리가 갈림
  - conn_fd를 부모는 바로 닫고, 자식은 작업 처리 후 닫음
    ``` mermaid
    sequenceDiagram
      participant C as 클라이언트
      participant P as 부모
      participant K as 자식

      C->>P: connect 후 accept로 conn_fd 생성
      P->>K: fork (fd 표 복제, 둘 다 conn_fd 보유)
      Note over P: close(conn_fd) 부모 번호표 닫음
      Note over P: accept로 복귀
      K->>C: read, 응답 write
      Note over K: close(conn_fd) 마지막 번호표
      K-->>C: FIN 전송
      Note over K: 프로세스 종료
    ```
- 장점 : 단순함.
- 단점 : 매 연결마다 프로세스 생성 비용이 듦

---

## 스레드 풀
- 스레드를 미리 N개 만들어두고, `accept`한 연결을 큐에 넣으면 놀고 있는 스레드가 꺼내 처리하는 구조
- 사용 이유
  - 요청이 몰리는 경우 스레드가 무한히 늘어날 수 있음.
  - 미리 만들어 둔 N개는 "제한"의 의미가 있음
- 의사 코드
  ``` c
  // 메인 스레드
  for (i = 0; i < N; i++) pthread_create(&tid[i], NULL, worker, NULL);
  while (1) {
      conn_fd = accept(listen_fd, ...);
      queue_push(conn_fd);          // 큐에 넣고 워커를 깨움
  }

  // 워커 스레드 (N개가 같은 함수를 실행)
  void *worker(void *arg) {
      while (1) {
          conn_fd = queue_pop();    // 비어 있으면 여기서 잠듦
          handle(conn_fd);
          close(conn_fd);           // 닫는 주체는 워커 하나
      }
  }
  ```
- lock 필요 : queue를 메인 스레드와 워커들이 동시에 건드림
  - mutex : 한 번에 한 스레드만 들어갈 수 있게 잠금
  - 조건 변수 : 큐가 비었을 때 워커를 잠재우고, push가 들어오면 깨우는 도구

- 한계
  - 최대 N 개라는 제약이 생김
  - 스레드들은 동일한 메모리를 공유하므로, 하나가 비정상 종료하면 전체 프로세스가 죽을 수 있음

---

## 이벤트 루프
- **이벤트 루프**
  - 스레드 하나가 `epoll_wait`로 준비된 연결만 골라서 당장 처리할 수 있는 만큼만 처리하고 다음 연결로 넘어가는 구조
  - 어디까지 처리했는지를 연결마다 직접 기억해야 함
- **non-blocking** 
  - fd에 `O_NONBLOCK` 플래그를 켬
  - 읽을 데이터가 없을 때 바로 -1을 반환, 이 때 `errno`는 `EAGAIN`
  - 보낼 공간이 부족할 때도 바로 -1을 반환, `errno`도 `EAGAIN`
- 상태 관리
  - blocking 방법으로 진행하면, 로컬 변수와 실행 위치가 당연히 보존됨 (block되고 다른 걸 실행하지 않음)
  - 이벤트 루프에서는, 스레드가 한 연결에서 중간에 떠나 다른 연결을 보고 돌아옴
  - 기억을 연결마다 **구조체로 저장**해야 함
    - epoll 등의 동시성 모델은 "준비된 fd 찾기" 비용을 줄여줌 
    - non-blocking과 **연결별 상태 구조체**가 **"흐름을 연결마다 두지 않기"**를 가능하게 하므로 빠지면 안 됨
    ``` c
    struct conn {
        int    fd;
        char   inbuf[8192];  size_t in_len;        // 지금까지 받은 바이트 (3-5, 5-4의 입력 버퍼)
        char  *outbuf;       size_t out_len, out_sent;  // 응답과 보낸 양 (4-6의 short write)
        int    state;        // 요청 읽는 중 / 응답 보내는 중 등
    };
    ```
- 루프 뼈대
  ``` c
  while (1) {
      n = epoll_wait(ep, ready, MAX, -1);
      for (i = 0; i < n; i++) {
          if (ready[i].data.fd == listen_fd) {
              conn_fd = accept(listen_fd, ...);
              set_nonblocking(conn_fd);
              epoll_ctl(ep, EPOLL_CTL_ADD, conn_fd, ...);   // 연결 구조체 생성
          } else if (읽기 준비) {
              r = read(fd, c->inbuf + c->in_len, ...);
              if (r > 0) { c->in_len += r; if (요청 완성) 응답 만들고 보내기 시작; }
              // r == -1 이고 EAGAIN이면 그냥 다음 연결로
          } else if (쓰기 준비) {
              // 남은 응답(out_sent부터) 이어서 write
          }
      }
  }
  ```

#### select / poll / epoll 비교
- `select`
  ``` c
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fd1, &rfds);  FD_SET(fd2, &rfds);     // 관심 fd를 비트 집합에 표시
  select(maxfd + 1, &rfds, NULL, NULL, &timeout);
  // 반환 후 rfds에는 "준비된 fd"만 남음 → 모든 fd를 FD_ISSET으로 하나씩 검사
  ```
  - fd_set은 fd 번호를 비트로 표시하는 고정 크기 집합.
  - 호출이 결과로 입력을 덮어씀 → 루프를 돌 때마다 rfds를 처음부터 다시 채움
  - 반환 후 어느 fd가 준비됐는지 찾으려고 전체를 검사
- `poll`
  ``` c
  struct pollfd fds[N];
  fds[0].fd = fd1;  fds[0].events = POLLIN;
  fds[1].fd = fd2;  fds[1].events = POLLIN;
  poll(fds, N, timeout_ms);
  // 반환 후 fds[i].revents에 준비된 이벤트가 기록됨
  ```
  - 비트 대신 배열을 씀. fd 번호 제한이 없음
  - 입력과 결과가 다른 필드라서, 매번 다시 채울 필요가 없음
  - 호출마다 배열 전체를 커널에 넘기고, 커널이 전부 훑고, 내가 또 전부 검사해야 함
- `epoll`
  - 코드는 루프 뼈대와 동일
  - 관심 목록은 커널에 한 번 등록 (`epoll_ctl`)
  - `epoll_wait`은 준비된 것만 돌려줌. 검사할 대상은 반환된 n 개 뿐
  - Linux 전용이기 때문에 이식성이 떨어짐 (다른 옵션들은 거의 모든 유닉스 지원)
  - `EPOLLIN`과 `EPOLLOUT` 이벤트 발생 기준
    ``` mermaid
    flowchart TB
      EP["epoll_ctl ADD / epoll_wait (공통 코드)"] --> OPS["fd의 file → f_op → poll 함수 호출"]
      OPS --> T1["TCP 소켓용 poll<br/>수신 버퍼, accept 큐, 송신 공간 검사"]
      OPS --> T2["파이프용 poll<br/>파이프 버퍼 검사"]
      OPS --> T3["그 외 객체용 poll"]
    ```
    - 소켓 종류별로 이벤트 발생 구현부가 다 다름
    - 예 : listen 소켓은 accept큐가 비어 있지 않으면 EPOLLIN
    - 예 : 파이프의 읽기 입구는 EPOLLIN 조건만 있고 EPOLLOUT 조건 없음
    - 예 : 파이프의 쓰기 입구는 EPOLLIN 조건은 없고 EPOLLOUT 조건만 있음

---

## 방식별 트레이드 오프와 C10K 문제
- **C10K 문제** : 동시 연결(Client) 1만(10k) 개
  - 당시 연결마다 프로세스나 스레드를 두는 방식 → 연결 1만 개 감당하기 어려움
  - 대부분의 연결이 놀고 있는데, 연결마다 실행 흐름을 하나씩 붙여두는 것이 감당되는가?
  - 느린 연결의 문제는 CPU가 아니라, 기다리는 동안 자리를 차지하는 것
- 연결 하나당 드는 비용
  | | 연결 하나가 차지하는 것 | 연결 10K일 때 |
  | --- | --- | --- |
  | 프로세스(fork) | 프로세스 1 개 (자체 메모리 공간, 커널 관리 항목) | 프로세스 10K |
  | 스레드 | 스레드 1 개 (스레드 전용 스택, 커널 관리 항목) | 풀이 N개로 제한 |
  | 이벤트 루프 | struct conn 하나와 epoll 등록 항목 | 구조체 1만 개, 스레드 1 개 |
  - 프로세스와 스레드는 **전환 비용**도 듦 (실행할 흐름을 바꿀 때마다 OS가 상태 저장 및 복원)
  - 이벤트 루프는 연결 전환이 "다음 구조체로 넘어가기"일 뿐임
- 트레이드 오프 정리
  | | 프로세스(fork) | 스레드 풀 | 이벤트 루프 |
  | --- | --- | --- | --- |
  | 코드 난이도 | 쉬움(순차 코드 그대로) | 중간(mutex, 조건변수) | 어려움(상태 관리 필요) |
  | 격리성 | 강함 (한 자식이 죽어도 안전) | 약함(하나가 죽으면 전체 영향) | 약함(한 스레드 뿐이라 전체) |
  | 연결 간 데이터 공유 | 어려움 (별도 통신 수단) | 쉬움 (대신 동기화 필요) | 쉬움 (단일 스레드면 동기화도 불필요) |
  | 대량의 유휴 연결 | 비용 큼 | 풀 크기가 한계 | 강함 |
  | 느린/오래 걸리는 처리 | 자식이 막혀도 무관 | 워커 하나만 막힘 | 루프 전체가 막힘 |
  | 여러 CPU 코어 활용 | 자연스러움 | 자연스러움 | 스레드 1 개면 코어 1 개 | 
  - 어느 한 쪽이 정답인 게 아니라, 연결의 성격에 따라 갈림
  - 연결이 적고 요청 하나가 무거움(CGI 등) → 프로세스, 스레드가 편함
  - 연결이 많고 대부분 유휴 → 이벤트 루프가 유리
  - 실무(nginx 등) → 조합이 흔함 (이벤트 루프 + 스레드 풀)

---

## epoll(Linux) vs kqueue(macOS)
- 공통점 : 관심 목록을 커널에 등록하고 준비된 것만 돌려받음
- 차이점 : 호출 이름과 구조체가 완전히 다름 → 한 코드로 양쪽에서 돌리려면, 감싸는 층이 필요함 (이식성 처리)
  - kqueue는 필터 방식임. 읽기 쓰기 뿐 아니라 파일 변경, 프로세스 종료, 시그널, 타이머까지 하나의 틀(`EVFILT_*`)로 들어옴.
  - 서로 다른 도구가 존재하는 이유 : 표준(POSIX)에 들어간 것은 `select`와 `poll` 뿐이라, 빠른 방식은 OS마다 따로 있음
- 호출 대응
  | 하는 일 | epoll (Linux) | kqueue (macOS) |
  | --- | --- | --- |
  | 인스턴스 생성 | `epoll_create1(0)` | `kqueue()` |
  | 등록·수정·삭제 | `epoll_ctl(ep, ADD/MOD/DEL, fd, &ev)` | `kevent()`에 변경 목록을 넘김 |
  | 대기 | `epoll_wait(ep, ready, n, timeout)` | `kevent()`에 결과 배열을 넘김 |
  | 읽기 이벤트 | `EPPOLLIN` | `EVFILT_READ` |
  | 쓰기 이벤트 | `EPOLLOUT` | `EVFILT_WRITE` | 
  | 사용자 데이터 | `data.ptr` | `udata` |
  ``` c
  // epoll: 등록과 대기가 다른 함수
  epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
  n = epoll_wait(ep, ready, MAX, -1);

  // kqueue: 한 함수 kevent로 등록과 대기를 모두 함
  struct kevent chg;
  EV_SET(&chg, fd, EVFILT_READ, EV_ADD, 0, 0, c);   // 마지막 c가 udata(내 구조체 포인터)
  kevent(kq, &chg, 1, NULL, 0, NULL);               // 등록만
  n = kevent(kq, NULL, 0, ready, MAX, NULL);        // 대기만
  ```

<br>
<br>

---

<p align="right">
  <a href="./06-static-dynamic.md">⬅ 이전 페이지</a>
  &nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="./08-proxy.md">다음 페이지 ➡</a>
</p>
