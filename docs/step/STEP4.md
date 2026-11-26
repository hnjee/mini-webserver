# Step 4. POST 본문 + keep-alive

## 목표

POST로 보낸 본문을 읽고, 연결 하나로 요청을 여러 개 받아요. 요구사항 ID는 R-05예요.

- `curl -i -X POST -d 'name=hyunji' http://localhost:8080/echo` → 보낸 본문이 그대로 돌아와요.
- `curl -v http://localhost:8080/index.html http://localhost:8080/style.css` → `Re-using existing connection`이 보여요.
- 아무것도 보내지 않는 연결은 5초 뒤에 닫아요.
- 본문이 너무 크면 413, URI가 너무 길면 414예요.

Step 3까지는 연결 하나에 요청 하나만 받았어요. 헤더 뒤에 딸려 온 바이트는 버렸고(결정 2-4), 응답한 뒤 바로 닫았어요(결정 2-11). Step 4는 이 세 가지를 바꿔요.

```
Step 3: accept → [헤더 읽기 → 응답] → close
Step 4: accept → [헤더 읽기 → 본문 읽기 → 응답 → 남은 바이트 당기기] 반복 → close
```

## 핵심 과제

1. **본문 읽기**: `Content-Length`만큼 본문을 모아요. 헤더와 같이 일부가 이미 와 있을 수 있어요.
2. **본문 담을 곳**: 8KB `buf`로는 작아서 본문은 따로 `malloc()`해요. 상한은 1MB예요.
3. **남은 바이트**: 요청 하나를 다 쓰고 남은 바이트는 다음 요청이에요. 버리지 않고 `buf` 앞으로 당겨요.
4. **연결 종료 조건**: HTTP 버전과 `Connection` 헤더로 계속 쓸지 정하고, 응답 헤더에도 그대로 써요.
5. **유휴 타임아웃**: 연결만 잡고 가만히 있는 클라이언트를 5초 뒤에 끊어요.
6. **크기 상한**: 본문 1MB(413), URI 2048바이트(414)를 넘으면 거절해요.

## 질문과 정리

### 주제 1. POST 본문 읽기

**Q. POST 요청은 GET과 어떻게 다른가?**

GET은 "이 파일 주세요"라서 헤더만 있어요. POST는 "이 내용을 받아 주세요"라서 헤더 뒤에 본문이 붙어 와요. 본문이 몇 바이트인지는 헤더의 `Content-Length`가 알려 줘요.

```
POST /echo HTTP/1.1\r\n
Host: localhost\r\n
Content-Length: 11\r\n      ← "본문은 11바이트예요"
\r\n                        ← 헤더 끝
name=hyunji                 ← 본문 (딱 11바이트)
```

지금 서버는 `\r\n\r\n`이 보이면 읽기를 멈추고 뒤에 온 `name=hyunji`는 버려요. Step 4에서는 헤더를 읽은 다음 `Content-Length`만큼 더 읽어요. 헤더를 `\r\n\r\n`이 나올 때까지 읽었던 것처럼, 본문은 11바이트가 모일 때까지 `read()`를 반복해요.

**Q. 헤더를 읽을 때 본문 일부가 같이 들어오면?**

`read()` 한 번에 헤더와 `name=hy`(7바이트)가 같이 왔다면, 남은 본문은 11 − 7 = 4바이트예요. 이미 받은 만큼 빼고 나머지만 더 읽어요.

### 주제 2. 본문 상한과 담을 곳

**Q. 본문이 엄청 크면?**

`Content-Length: 1000000000`(1GB)이 와도 `malloc(1GB)`를 해 주면, 이런 요청 몇 개로 서버 메모리가 바닥나요. 그래서 상한을 정하고, 넘으면 본문을 읽지도 않고 **413 Content Too Large**로 거절해요. `Content-Length`는 헤더에 있어서 본문이 오기 전에 크기를 알 수 있어요.

**Q. 상한은 보통 얼마인가?**

"이 서버가 POST로 무엇을 받을까"를 기준으로 정해요.

| 서버 | 기본 본문 상한 |
| ---- | ---- |
| nginx | 1MB (`client_max_body_size`) |
| Express (Node.js) | 100KB (JSON 본문) |
| Apache | 1GB |

8KB `buf` 안에 본문까지 담으면(A) 단순하지만 웹서버로 쓰기엔 너무 작아요. 그래서 nginx 기본값과 같은 **1MB**로 하고, 본문은 따로 `malloc()`해요(B).

**Q. 본문은 어떻게 모으나?**

헤더 읽기는 지금과 같아요. 달라지는 건 헤더를 찾은 다음이에요.

```
1. 헤더 읽기 (지금과 같음)
   buf: [ 헤더 \r\n\r\n | name=hy ]        ← 본문 7바이트가 딸려 왔어요

2. Content-Length(11)만큼 malloc
   body: [ _ _ _ _ _ _ _ _ _ _ _ ]

3. buf에 딸려 온 7바이트를 body로 복사
   body: [ n a m e = h y _ _ _ _ ]

4. 나머지 4바이트는 body에 바로 read()
   read(client_fd, body + 7, 11 - 7)
   body: [ n a m e = h y u n j i ]
```

나머지 본문은 `buf`를 거치지 않고 `body`에 바로 읽어요. 8KB `buf`를 거치면 큰 본문은 여러 번 복사해야 하니까요. `read()`는 매번 딱 요청한 만큼 읽는 게 아니라 그때까지 도착한 만큼만 돌려줘서, 모일 때까지 반복해요.

### 주제 3. 남은 바이트 → 다음 요청

**Q. 본문 뒤에 바이트가 더 와 있으면?**

keep-alive 연결에서는 클라이언트가 다음 요청을 바로 이어 보낼 수 있어요. 그러면 `read()` 한 번에 요청 1의 끝과 요청 2의 앞부분이 같이 들어와요.

```
당기기 전: [ 요청1 헤더 | name=hyunji | GET /style.css ... ]
                                       ↑ 여기부터는 다음 요청

당긴 후:   [ GET /style.css ... ]
           ↑ len = 남은 바이트 수
```

남은 바이트를 `buf` 맨 앞으로 당기면 다음 요청도 "`buf` 맨 앞부터 헤더"로 똑같이 처리할 수 있어요. 복사하는 곳과 복사될 곳이 같은 `buf` 안에서 겹칠 수 있어서 `memcpy()`가 아니라 `memmove()`를 써요.

**Q. 언제 당기나?**

`req.method`, `req.uri` 같은 포인터가 `buf` 안을 가리켜요(결정 2-5). 응답을 보내기 전에 당기면 그 자리에 다음 요청 글자가 덮여요. 그래서 **응답을 다 보낸 다음에** 당겨요.

```
1. 요청 처리 (buf 안의 req 포인터 사용)
2. 응답 보내기 끝
3. 다 쓴 부분(헤더 + 본문)만큼 당기기
4. 다음 요청으로 (len = 0으로 지우지 않고 남은 바이트부터 시작)
```

**Q. 당겨 온 바이트에 다음 요청이 통째로 들어 있으면?**

지금 루프는 `read()`보다 `request_find_end()`를 먼저 해서, `read()` 없이 바로 처리해요. 순서가 반대면 클라이언트는 다 보냈다고 기다리고 서버는 `read()`에서 기다려서 둘 다 멈춰요.

### 주제 4. 연결을 언제 닫나

**Q. 같은 연결로 계속 받으면 언제 닫나?**

| 상황 | 닫나요? |
| ---- | ---- |
| 클라이언트가 `Connection: close`를 보냄 | 닫아요 |
| 클라이언트가 끊음 (`read()` == 0) | 닫아요 |
| 요청 경계를 믿을 수 없는 에러 (400, 413 등) | 닫아요 |
| 아무것도 안 보내고 가만히 있음 | 타임아웃이 지나면 닫아요 |
| `Connection` 헤더가 없음 | HTTP 버전마다 기본값이 달라요 |

- **HTTP/1.0**: 기본이 "응답하면 닫기". 계속 쓰고 싶으면 클라이언트가 `Connection: keep-alive`를 보내요.
- **HTTP/1.1**: 기본이 "계속 쓰기". 닫고 싶으면 클라이언트가 `Connection: close`를 보내요.

HTTP/1.1이 나올 때쯤 웹 페이지 하나에 CSS, JS, 이미지가 수십 개씩 붙었어요. 파일마다 TCP 연결을 새로 맺으면(3-way handshake) 느려서 기본값을 "계속 쓰기"로 바꿨어요.

**Q. 응답의 `Connection` 헤더는?**

서버가 이 응답 뒤에 **실제로 할 일**을 써요. 계속 쓸 거면 `keep-alive`, 닫을 거면 `close`예요. 그래서 응답을 보내기 전에 닫을지부터 정해요.

```
닫을지 정하기 = 클라이언트가 원하는 것 (버전 기본값 + Connection 헤더)
              + 서버 사정 (경계를 믿을 수 없는 에러면 클라이언트가 원해도 닫음)
→ 응답 헤더에 그대로 쓰기 → 응답 보내기 → 정한 대로 닫거나 다음 요청으로
```

지금 `send_header()`는 항상 `close`를 써서, 닫을지 여부를 인자로 받게 바꿔요.

### 주제 5. 유휴 타임아웃

**Q. 클라이언트가 요청 하나 하고 가만히 있으면?**

서버는 다음 요청을 기다리며 `read()`에서 멈춰요. 지금 서버는 반복 서버(결정 1-5)라서 그동안 다른 클라이언트는 `accept()`도 못 받아요.

```
클라이언트 A: 요청 하나 보내고 가만히 있음  ← 서버가 여기서 read()로 멈춤
클라이언트 B: 요청 보냄                     ← 계속 기다림
```

**Q. 타임아웃은 몇 초로 하나?**

짧으면 느린 클라이언트가 끊기고, 길면 다른 사람이 오래 기다려요.

| 서버 | 기본 keep-alive 타임아웃 |
| ---- | ---- |
| Apache | 5초 |
| nginx | 75초 |

Apache는 연결 하나에 일꾼(프로세스나 스레드) 하나를 붙여서, 가만히 있는 연결도 일꾼을 붙잡아요. 그래서 짧게 둬요. nginx는 `epoll`로 일꾼 하나가 연결 수천 개를 봐서 길게 둬도 돼요. 우리는 일꾼이 하나뿐이라 **5초**로 해요. Step 5에서 동시성 방식을 고르면 다시 볼 수 있어요.

**Q. 요청 중간에 멈추면?**

헤더를 반쯤 보내거나 본문을 덜 보내고 멈춰도 서버가 묶여요. 일부러 아주 천천히 보내서 서버를 묶는 공격도 있어요(Slowloris). 그래서 모든 `read()`에 타임아웃을 걸어요.

| 언제 타임아웃이 났나 | 처리 | 이유 |
| ---- | ---- | ---- |
| 요청과 요청 사이 (`buf`가 비어 있음) | 그냥 닫기 | 보낸 요청이 없는데 응답을 보내면 클라이언트가 안 보낸 요청의 응답을 받아요 |
| 요청 중간 (헤더나 본문 일부를 받음) | **408 Request Timeout** 후 닫기 | 보내는 중이었으니 왜 실패했는지 알려 줘요 |

**Q. 타임아웃은 어떻게 만드나?**

| 방법 | 장점 | 단점 |
| ---- | ---- | ---- |
| (A) `SO_RCVTIMEO` | `accept()` 직후 한 번 걸면 모든 `read()`에 적용돼요. 코드가 거의 안 바뀌어요 | `read()` 한 번마다 5초라, 4초마다 1바이트씩 보내면 안 끊겨요 |
| (B) `read()` 전에 `poll()` | 남은 시간을 계산하면 "요청 전체에 10초"도 만들 수 있어요 | `read()`마다 `poll()`을 붙여야 해요 |

(A)를 골랐어요. 가만히 있는 연결을 끊는 목적엔 충분하고, Step 5의 동시성 방식에 따라 타임아웃 방식도 다시 보게 돼요. 아주 천천히 보내는 클라이언트를 완전히 막는 건 Step 7에서 다뤄요. 시간이 지나면 `read()`가 -1을 돌려주고 `errno`가 `EAGAIN`(또는 `EWOULDBLOCK`)이 돼요.

### 주제 6. 크기 상한

**Q. 상한은 어디에 어떻게 두나?**

| 대상 | 상한 | 넘으면 | 언제 판정하나 |
| ---- | ---- | ---- | ---- |
| 헤더 전체 | 8KB (`buf` 크기) | 431 | 헤더를 읽는 중 (결정 2-2) |
| URI | 2048바이트 | 414 | 파서가 요청 라인을 나눈 직후 |
| 본문 | 1MB | 413 | 헤더를 파싱한 직후, 본문을 읽기 전 |

413은 본문을 읽지 않고 보내요. 그러면 `buf` 뒤의 바이트는 다음 요청이 아니라 읽다 만 본문이라서 연결을 닫아요.

**Q. URI 414는 왜 파서에서 하나?**

Step 3은 파일 경로가 `PATH_MAX`를 넘을 때만 414를 보내요. Step 6의 `/api/*`는 파일이 아니라 백엔드로 가서 이 검사를 안 거쳐요. 파서에서 판정하면 정적이든 프록시든 같은 규칙이고 단위 테스트도 할 수 있어요. 2048은 예전 IE의 URL 한도가 약 2KB여서 "URL은 2KB 안으로"가 관례가 된 숫자예요. Step 3의 `PATH_MAX` 검사는 그대로 둬요. 루트 경로가 길면 URI가 2048 안이어도 합친 경로가 넘을 수 있어요.

### 주제 7. POST는 무엇을 하나

**Q. `/echo`가 아닌 곳에 POST하면?**

| 요청 | 처리 |
| ---- | ---- |
| `POST /echo` | 받은 본문을 그대로 돌려줘요 |
| `POST /다른경로` | **405 Method Not Allowed** |
| `GET /echo` | 정적 파일 처리 (`www/echo`가 없으니 404) |

정적 파일은 POST로 받을 수 없어서 405예요. 501은 "서버가 아예 모르는 메서드"라 POST엔 맞지 않아요. Step 6에서 `/api/*`의 POST는 백엔드로 넘어가요.

`/echo` 응답의 `Content-Type`은 `application/octet-stream`이에요. 받은 바이트를 그대로 돌려줄 뿐이라 무슨 내용인지 서버가 보장할 수 없어요.

### 구현하면서 생긴 질문

**Q. 2000자 URI는 414 상한(2048) 안인데 왜 500이 나오나?**

`/aaa...a`(2000자)는 파서를 통과하지만, 파일 이름 한 조각이 1999자예요. 리눅스 파일 시스템은 이름 한 조각을 `NAME_MAX`(보통 255바이트)까지만 허용해서 `realpath()`가 `ENAMETOOLONG`으로 실패해요. 결정 3-15의 표에 이 `errno`가 없어서 "그 밖의 실패 → 500"이 됐어요. 서버 문제가 아니라 요청한 이름이 너무 긴 거라서 414로 바꿨어요. 조각마다 255자 안이면(`/aaa.../aaa...`) 2000자 URI도 404예요.

## 이 단계에서 정한 것

| #&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; | 결정 | 이유 |
| - | ---- | ---- |
| 4-1 | 헤더를 파싱한 뒤 `Content-Length`만큼 본문을 읽는다. `Content-Length`가 없으면 본문은 0바이트다. 메서드와 상관없이 읽는다. | 본문을 끝까지 읽어야 다음 요청이 어디서 시작하는지 알아요. `Content-Length`도 `Transfer-Encoding`도 없으면 본문 길이는 0이에요 (RFC 9112). |
| 4-2 | 본문 상한은 1MB(`REQUEST_BODY_MAX`)다. 넘으면 본문을 읽지 않고 413을 보내고 연결을 닫는다. | nginx 기본값과 같아요. 크기는 헤더에서 미리 알 수 있어서 읽을 필요가 없어요. 읽지 않은 본문이 남아 있으니 다음 요청을 찾을 수 없어요. |
| 4-3 | 본문은 `Content-Length`만큼 `malloc()`한 별도 버퍼에 담는다. `buf`에 딸려 온 부분을 복사하고, 나머지는 본문 버퍼에 바로 `read()`한다. 0바이트면 `malloc()`하지 않는다. 응답을 보낸 뒤 `free()`한다. `malloc()`이 실패하면 500을 보내고 닫는다. | 8KB `buf`로는 웹서버로 쓰기에 본문이 너무 작아요. 나머지를 `buf`를 거치지 않고 읽으면 복사가 한 번으로 끝나요. |
| 4-4 | `Transfer-Encoding` 헤더가 있으면 501이다. | chunked 본문은 지원하지 않아요. 무시하고 넘어가면 본문을 다음 요청으로 잘못 읽어요. 지원하지 않는 전송 코딩은 501로 답해요 (RFC 9112). |
| 4-5 | 응답을 다 보낸 뒤 사용한 바이트(헤더 + 본문)만큼 `memmove()`로 `buf`를 앞으로 당기고, `len`을 남은 길이로 둔다. 다음 요청은 `read()` 전에 `request_find_end()`부터 한다. | 결정 2-4를 바꿔요. `req` 포인터가 `buf`를 가리키니 응답이 끝나야 당길 수 있어요(결정 2-5). 남은 바이트에 다음 요청이 통째로 있으면 `read()` 없이 처리해야 멈추지 않아요. |
| 4-6 | HTTP/1.1은 기본으로 연결을 유지하고 `Connection: close`면 닫는다. HTTP/1.0은 기본으로 닫고 `Connection: keep-alive`면 유지한다. 값은 대소문자를 구분하지 않는다. 판정은 `request.c`의 `request_keep_alive(&req)`로 하고 단위 테스트한다. | HTTP 버전별 기본값이에요 (RFC 9112). 소켓 없이 판정할 수 있어서 파서 쪽에 두고 테스트해요. |
| 4-7 | 요청 경계를 믿을 수 없는 경우(400, 408, 413, 414, 431, 501, 505, 본문 `malloc()` 실패, 응답 전송 실패)는 응답 뒤에 닫는다. 요청을 끝까지 받은 뒤의 결과(404, 403, 405, 정적 파일의 500)는 4-6을 따른다. | 결정 2-11을 바꿔요. 요청을 끝까지 읽었으면 남은 바이트는 다음 요청이 맞아서 연결을 이어도 돼요. 브라우저의 `favicon.ico` 404 하나로 연결을 끊지 않아요. |
| 4-8 | 응답의 `Connection` 헤더는 서버가 실제로 할 일(`keep-alive`/`close`)과 맞춘다. `send_header()`, `send_response()`, `send_error()`에 `keep_alive` 인자를 더한다. | 클라이언트는 이 헤더를 보고 다음 요청을 같은 연결로 보낼지 정해요. 결정 3-10의 `send_header()` 모양이 바뀌어요. |
| 4-9 | `accept()` 직후 `SO_RCVTIMEO`로 5초 타임아웃을 건다. 헤더와 본문의 모든 `read()`에 적용된다. | 반복 서버라 가만히 있는 연결 하나가 모두를 막아요. Apache 기본값과 같은 5초예요. 한 번 걸면 되고 코드가 거의 안 바뀌어요. |
| 4-10 | 타임아웃이 났을 때 `buf`가 비어 있고 본문을 읽는 중이 아니면 응답 없이 닫는다. 요청 중간이면 408을 보내고 닫는다. | 보낸 요청이 없는데 응답하면 클라이언트가 안 보낸 요청의 응답을 받아요. 보내는 중이었다면 실패 이유를 알려 줘요. |
| 4-11 | URI 상한은 2048바이트(`REQUEST_URI_MAX`)이고, 파서가 요청 라인을 나눈 직후 414를 돌려준다. 판정 순서는 요청 라인 형식(400) → URI 길이(414) → 헤더 형식(400) → 메서드와 `Transfer-Encoding`(501) → 버전(505)이다. 결정 3-15의 `PATH_MAX` 414는 그대로 둔다. | 정적 파일과 Step 6의 프록시가 같은 규칙을 써요. 2048은 URL 길이의 오랜 관례예요. 루트가 길면 합친 경로가 `PATH_MAX`를 넘을 수 있어요. |
| 4-12 | `POST /echo`(쿼리를 뗀 경로가 정확히 `/echo`)는 받은 본문을 그대로 `application/octet-stream`으로 200에 보낸다. | Step 4의 완료 기준이에요. 받은 바이트가 무슨 내용인지 서버가 보장할 수 없어요. |
| 4-13 | 그 밖의 `POST`는 405이고 응답에 `Allow: GET`을 넣는다. `GET /echo`는 정적 파일로 처리한다. | 결정 3-16을 바꿔요. 정적 파일은 POST로 받을 수 없어요. 501은 서버가 모르는 메서드라 POST엔 맞지 않아요. 405에는 `Allow` 헤더가 필수예요 (RFC 9110). |
| 4-14 | `serve_static(client_fd, root, &req, &keep_alive)`로 바꾼다. 받은 `keep_alive`로 응답 헤더를 쓰고, 헤더를 보낸 뒤 전송이 실패하면 0으로 바꿔 호출한 쪽이 닫게 한다. | 결정 3-9처럼 본문을 다 못 보내면 `Content-Length`가 어긋나서 연결을 이을 수 없어요. |
| 4-15 | 연결 처리는 `server.c`의 `handle_connection()`이 요청을 반복하고, 헤더 읽기는 `read_header()`, 본문 읽기는 `read_body()`로 나눈다. 연결을 닫을 때 이유(`client closed`, `connection close`, `idle timeout`, `error`)를 로그에 붙인다. | 반복 흐름과 읽기 처리를 나눠야 읽기 쉬워요. 로그로 keep-alive 재사용과 타임아웃을 눈으로 확인해요. |
| 4-16 | 응답 쓰기 타임아웃(`SO_SNDTIMEO`)과 아주 천천히 보내는 클라이언트 방어는 Step 7로 미룬다. | 받지 않는 클라이언트에 쓰면 `write()`가 멈출 수 있다는 걸 알고 넘어가요. 안정성은 Step 7 요구사항이에요. |
| 4-17 | 정적 파일에서 `ENAMETOOLONG`은 414다. | 결정 3-15에 없어서 500이 나왔어요. 파일 이름 한 조각이 `NAME_MAX`(255)를 넘는 건 서버 문제가 아니라 요청한 이름이 너무 긴 거예요. |

## 만드는 파일과 함수

| 파일 | 내용 |
| ---- | ---- |
| `src/request.h`, `src/request.c` | `REQUEST_URI_MAX`, `REQUEST_BODY_MAX` 추가. struct에 `transfer_encoding` 추가. URI 414, `Transfer-Encoding` 501 판정. `request_keep_alive()` 추가 |
| `src/response.h`, `src/response.c` | `send_header()`, `send_response()`, `send_error()`에 `keep_alive` 인자. `status_text()`에 405, 408, 413 추가. 405면 `Allow: GET` 헤더 |
| `src/static.h`, `src/static.c` | `serve_static()`이 `int *keep_alive`를 받아요. `ENAMETOOLONG` → 414 |
| `src/server.h`, `src/server.c` | `handle_http()`를 `handle_connection()`으로 바꿔 요청을 반복해요 |
| `tests/test_request.c` | 414, `Transfer-Encoding` 501, `request_keep_alive()` 테스트 추가 |

`server.c`의 함수예요.

| 함수 | 하는 일 |
| ---- | ------- |
| `handle_connection(client_fd, root)` | `SO_RCVTIMEO`를 걸고 요청을 반복해요: `read_header()` → `parse_request()` → 413 판정 → `read_body()` → 닫을지 정하기 → 응답 → `free(body)` → 로그 → 닫거나 `memmove()`로 당기기 |
| `read_header(fd, buf, &len, &end)` | `\r\n\r\n`을 찾을 때까지 `read()`를 반복해요. 결과는 성공, 상대가 끊음, 유휴 타임아웃(그냥 닫기), 요청 중 타임아웃(408), 버퍼 가득(431), 읽기 에러 중 하나예요 |
| `read_body(fd, buf, len, end, content_length, &body)` | `content_length`만큼 `malloc()` → `buf`에 딸려 온 부분 복사 → 나머지를 `body`에 바로 `read()`해요. 결과는 성공, 상대가 끊음, 타임아웃(408), `malloc()` 실패(500) 중 하나예요 |

`request.c`에 더하는 함수예요.

| 함수 | 하는 일 |
| ---- | ------- |
| `request_keep_alive(&req)` | 4-6 규칙대로 연결을 유지하면 1, 닫으면 0을 돌려줘요 |

## 완료 테스트

모든 명령은 컨테이너 안에서 실행해요. 프롬프트가 `dev@xxxx:/app$`인지 먼저 확인해요. 이전 Step의 서버가 켜져 있으면 Ctrl+C로 끄고 다시 켜요.

```sh
# 터미널 A
make && ./webserver -p 8080 -r ./www
```

**1. 단위 테스트**

```sh
# 터미널 B
make test
# → test_request: 67 checks, 0 failures
# → test_mime: 24 checks, 0 failures
```

**2. echo** (완료 기준)

```sh
curl -i -X POST -d 'name=hyunji' http://localhost:8080/echo
# → 200, Content-Type: application/octet-stream, Connection: keep-alive, 본문 name=hyunji
```

**3. 나뉘어 온 본문**

```sh
(printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\nConnection: close\r\n\r\nname=hy'; sleep 1; printf 'unji') | nc localhost 8080
# → 1초 뒤 본문 name=hyunji, Connection: close
```

**4. 큰 본문**

```sh
head -c 1048576 /dev/urandom > /tmp/1mb.bin
curl -s -H 'Expect:' --data-binary @/tmp/1mb.bin http://localhost:8080/echo | cmp - /tmp/1mb.bin && echo "1MB same"
# → 1MB same (상한과 같은 크기는 받아요)

printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n' | nc localhost 8080
# → 413, Connection: close
```

`curl`은 큰 본문을 보낼 때 `Expect: 100-continue`를 붙이고 서버의 `100 Continue`를 기다려요. 우리 서버는 보내지 않아서 1초쯤 기다렸다가 보내요. `-H 'Expect:'`로 이 헤더를 빼요.

**5. keep-alive** (완료 기준)

```sh
curl -v http://localhost:8080/index.html http://localhost:8080/style.css 2>&1 | grep -i -E 're-?using|connection:'
# → Re-using existing connection ..., Connection: keep-alive
# 터미널 A: connected 한 번, request 두 줄, closed 한 번
```

**6. 남은 바이트 (한 번에 요청 두 개)**

```sh
printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhelloGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | nc localhost 8080
# → 응답 두 개: 본문 hello인 200, 그 뒤 style.css의 200 (Connection: close)
```

**7. HTTP/1.0**

```sh
curl -s -0 -D - -o /dev/null http://localhost:8080/ | grep -i connection                              # → close
curl -s -0 -H 'Connection: keep-alive' -D - -o /dev/null http://localhost:8080/ | grep -i connection  # → keep-alive
curl -s -H 'Connection: close' -D - -o /dev/null http://localhost:8080/ | grep -i connection          # → close
```

**8. 유휴 타임아웃**

```sh
nc localhost 8080            # 아무것도 입력하지 않고 기다려요
# 터미널 A: 5초 뒤 closed: ... (idle timeout)
# nc는 서버가 닫아도 입력이 끝날 때까지 남아 있을 수 있어요. Ctrl+C로 끝내요

(printf 'GET / HTTP/1.1\r\n'; sleep 7) | nc localhost 8080
# → 5초 뒤 408, Connection: close
```

`nc`로 접속만 해 둔 동안 다른 터미널에서 `curl http://localhost:8080/`을 하면 최대 5초 기다렸다가 응답이 와요. 반복 서버라서 그래요. Step 5에서 해결해요.

**9. 크기 상한과 405, 501**

```sh
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/$(head -c 3000 /dev/zero | tr '\0' a)   # → 414 (파서, 4-11)
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/$(head -c 2000 /dev/zero | tr '\0' a)   # → 414 (이름 한 조각 > 255, 4-17)
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/$(head -c 255 /dev/zero | tr '\0' a)    # → 404
curl -i -X POST -d x http://localhost:8080/index.html          # → 405, Allow: GET, Connection: keep-alive
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/echo                  # → 404
curl -s -o /dev/null -w "%{http_code}\n" -H 'Transfer-Encoding: chunked' -d x http://localhost:8080/echo   # → 501
```

**10. Step 3 회귀**

STEP3.md의 완료 테스트 3~6을 다시 실행해요. 결과가 같아야 해요.

**11. 메모리 검사**

```sh
# 터미널 A: 서버를 끄고 디버그 빌드로 다시 실행
make debug && ./webserver-debug -p 8080 -r ./www
# 터미널 B: 테스트 2~9를 다시 실행
# → 터미널 A에 ASan 에러가 나오지 않아요 (본문 malloc/free 누수 포함)
```
