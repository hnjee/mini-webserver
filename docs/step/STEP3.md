# Step 3. 정적 파일 서빙

## 목표

URI가 가리키는 루트 디렉터리 안의 진짜 파일을 보내요. 요구사항 ID는 R-04예요.

- 브라우저에서 HTML, CSS, JS, PNG, JPG가 모두 제대로 보여요.
- 없는 파일은 404, 읽기 권한이 없는 `secret.txt`는 403이에요.
- `/../../etc/passwd`처럼 루트 밖으로 나가려는 요청은 403이에요.
- MIME 매핑을 `make test`로 단위 테스트해요.

Step 2에서는 어떤 요청이든 `<h1>Hello, world!</h1>`를 보냈어요. 이제 `GET`은 파일을 보내요.

```
GET /style.css  →  ./www/style.css 를 읽어서  →  200 + Content-Type: text/css + 파일 내용
```

## 핵심 과제

1. **URI → 파일 경로**: 루트 + URI로 경로를 만들어요. 디렉터리면 `index.html`을 붙여요.
2. **경로 탈출 막기**: `..`이나 심볼릭 링크로 루트 밖의 파일을 읽지 못하게 해요.
3. **파일 보내기**: 5MB `large.bin`도, `'\0'`이 섞인 PNG도 깨지지 않게 보내요.
4. **MIME 타입**: 확장자로 `Content-Type`을 정해요.
5. **에러 판정**: 404, 403, 500을 실패 이유(`errno`)로 골라요.

## 질문과 정리

### 주제 1. URI를 파일 경로로 바꾸기

**Q. URI를 파일 경로로 바꾸는 것도 서버가 해야 하나?**

네. URI는 "이 이름의 자원을 주세요"라는 요청일 뿐이고, 그게 디스크의 어느 파일인지는 서버만 알아요. `-r ./www`로 켜면 `/style.css`는 `./www/style.css`고, `-r /var/site`로 켜면 `/var/site/style.css`예요. Step 6의 `/api/*`처럼 파일이 아니라 백엔드로 보내는 URI도 있어요. nginx 설정의 `root /var/www;`가 하는 일이 바로 이 "루트 + URI" 변환이에요.

**Q. URI를 그대로 이어 붙이면 되나?**

거의 그렇지만 몇 가지를 처리해야 해요.

| 요청 URI | 그냥 붙이면 | 처리 |
| ---- | ---- | ---- |
| `/style.css` | `./www/style.css` | 그대로 |
| `/` | `./www/` (디렉터리) | `index.html`을 붙여요 |
| `/index.html?v=2` | `./www/index.html?v=2` | `?` 뒤는 쿼리 문자열이라 떼요 |
| `/my%20file.html` | `./www/my%20file.html` | 디코딩하지 않아요 |

`%XX`를 디코딩하면 `%2e%2e`가 `..`로 바뀌어서, 경로 탈출 검사를 디코딩 뒤에 해야 하는 함정이 생겨요. 디코딩하지 않으면 `%2e%2e`는 그냥 이상한 이름이라 404로 끝나요. 테스트 파일 이름에 공백이나 한글이 없어서 지금은 필요 없어요.

### 주제 2. 경로 탈출 막기

**Q. 경로 탈출은 어떻게 막나?**

서버가 경로를 직접 만들기 때문에 `..`이 들어오면 루트 밖으로 나갈 수 있어요.

```
요청:      GET /../../etc/passwd
이어 붙임:  ./www/../../etc/passwd
실제 위치:  /etc/passwd        ← www 밖
```

방법은 두 가지예요.

- (A) 문자열 검사: URI에 `..` 조각이 있으면 403이에요. 단순하지만, 루트 안에 바깥을 가리키는 심볼릭 링크가 있으면 못 막아요.
- (B) `realpath()`: `..`, `.`, `//`, 심볼릭 링크를 모두 풀어 진짜 절대 경로를 구하고, 그게 루트 안인지 봐요.

```
realpath("./www")                    → /app/www           (서버 시작할 때 한 번)
realpath("./www/../../etc/passwd")   → /etc/passwd        → 루트 밖 → 403
realpath("./www/style.css")          → /app/www/style.css → 루트 안 → OK
realpath("./www/nope.html")          → 실패 (ENOENT)      → 404
```

(B)는 글자가 아니라 실제로 가리키는 곳을 봐서 어떤 식으로 `..`을 섞어도 잡혀요. 함정은 `/app/www2`도 문자열로는 `/app/www`로 시작한다는 거예요. 그래서 "루트와 같거나, 루트 + `/`로 시작하는가"로 검사해요. `curl`은 보내기 전에 `..`을 정리해 버려서 테스트에 `--path-as-is`를 붙여요.

### 주제 3. 파일 보내기

**Q. 큰 파일은 어떻게 보내나?**

| 방법 | 장점 | 단점 |
| ---- | ---- | ---- |
| (A) 통째로 `malloc()` → 한 번에 | `send_response()`를 그대로 써요 | `large.bin` 하나에 5MB. Step 5에서 100명이면 500MB |
| (B) 64KB씩 `read()` → `write_all()` 반복 | 파일 크기와 상관없이 메모리 64KB | 헤더를 먼저 보내는 함수가 따로 필요해요 |
| (C) `sendfile()` | 커널이 바로 보내서 가장 빨라요 | 리눅스 전용, POSIX가 아니에요 |

(B)를 골랐어요. 그래서 `send_response()`에서 헤더 보내는 부분을 `send_header()`로 뗐어요.

**Q. 바이너리가 깨지지 않으려면?**

PNG, JPG 안에는 `'\0'` 바이트가 섞여 있어요. `strlen()`은 첫 `'\0'`에서 멈추고, `fgets()`나 `printf("%s")`는 줄이나 `'\0'` 단위로 다뤄요. 그래서 길이는 `stat()`의 `st_size`로, 읽고 쓰기는 바이트 수만 보는 `read()`/`write()`로 해요.

**Q. 보내는 중에 실패하면?**

헤더(`200 OK`)를 이미 보냈으면 500으로 바꿀 수 없어요. 그냥 연결을 닫아요. 클라이언트는 `Content-Length`보다 덜 받았으니 실패한 걸 알아요.

### 주제 4. MIME 타입

**Q. MIME 타입은 왜 필요한가?**

브라우저는 파일 이름이 아니라 `Content-Type`을 보고 내용을 어떻게 다룰지 정해요. CSS를 `text/plain`으로 보내면 스타일로 쓰지 않고, 모르는 걸 `application/octet-stream`으로 보내면 다운로드해요. 텍스트 타입에는 `charset=utf-8`을 붙여 한글이 깨지지 않게 해요. `.js`는 지금 표준(RFC 9239)대로 `text/javascript`예요.

### 주제 5. 404, 403, 500 판정

**Q. `secret.txt`는 어느 단계에서 403이 되나?**

`realpath()`는 경로를 따라가기만 해서 디렉터리에 들어갈 권한만 있으면 성공해요. 파일 내용을 읽는 권한은 `open()`할 때 검사해요. 그래서 `secret.txt`는 `open()`의 `EACCES`로 403이 돼요. 컨테이너를 root가 아닌 `dev`로 실행하는 이유(결정 0-13)도 이거예요. root는 권한 000 파일도 열 수 있어요.

**Q. 403, 404, 500은 어떻게 다른가?**

404는 "없어요", 403은 "있지만 줄 수 없어요"예요. 4xx는 요청 쪽 문제고, 500은 fd가 바닥나는 것처럼 클라이언트 잘못이 아닌 서버 쪽 문제일 때만 써요.

### 구현하면서 생긴 질문

**Q. `realpath()`가 선언되지 않았다는 경고가 나요.**

glibc는 `realpath()`를 X/Open(XSI) 기능으로 분류해서 `_POSIX_C_SOURCE`만으로는 선언해 주지 않아요. 선언이 없으면 컴파일러가 반환형을 `int`로 짐작해서, 64비트에서는 포인터가 잘릴 수 있어요. 컴파일 옵션에 `-D_XOPEN_SOURCE=700`을 더했어요. POSIX.1-2008을 포함하는 상위 집합이라 쓸 수 있는 범위만 넓어져요.

## 이 단계에서 정한 것

| # | 결정 | 이유 |
| - | ---- | ---- |
| 3-1 | 파일 경로는 루트의 절대 경로 + URI로 만든다. URI의 `?` 뒤(쿼리 문자열)는 뗀다. | 쿼리 문자열은 파일 이름이 아니에요. |
| 3-2 | URI의 `%XX`는 디코딩하지 않는다. | 테스트 파일 이름에 필요 없어요. 디코딩하면 `%2e%2e`가 `..`이 되어 경로 탈출 검사를 디코딩 뒤에 해야 하는 함정이 생겨요. |
| 3-3 | 디렉터리를 요청하면 그 안의 `index.html`을 보낸다. 없으면 404다. 디렉터리 목록은 보여 주지 않는다. | 디렉터리 요청에 `index.html`을 보내는 건 과제 요구사항이에요. (DESIGN.md 제안과 같음) 목록 기능은 요구사항에 없어요. |
| 3-4 | 경로 탈출은 `realpath()`로 실제 위치를 구한 뒤 루트와 같거나 루트 + `/`로 시작하는지로 막는다. 밖이면 403이다. | `..`, `//`, 심볼릭 링크를 모두 풀어서 실제로 가리키는 곳을 봐요. `/app/www2`가 `/app/www` 안으로 잘못 판정되지 않게 `/`까지 확인해요. |
| 3-5 | 루트의 `realpath()`는 서버 시작할 때 한 번 구한다. 실패하면 `perror()`를 출력하고 종료 코드 1로 끝낸다. | 요청마다 구할 필요가 없어요. 1-8처럼 준비 단계 실패는 서버를 켜지 않아요. |
| 3-6 | 디렉터리에 `index.html`을 붙인 뒤에는 `realpath()`부터 같은 검사를 한 번 더 한다. | `index.html`이 바깥을 가리키는 심볼릭 링크일 수도 있어요. |
| 3-7 | 파일은 헤더를 먼저 보내고 64KB 지역 버퍼로 `read()` → `write_all()`을 반복해 보낸다. `sendfile()`은 쓰지 않는다. | 파일 크기와 상관없이 메모리가 일정해요. `sendfile()`은 리눅스 전용이라 C + POSIX 조건에 맞지 않아요. |
| 3-8 | `Content-Length`는 연 파일의 `fstat()` `st_size`로 정하고, 파일은 `read()`/`write()`로만 다룬다. | `strlen()`, `fgets()` 같은 문자열 함수는 `'\0'`이 섞인 바이너리를 깨뜨려요. |
| 3-9 | 헤더를 보낸 뒤 실패하면 응답을 바꾸지 않고 연결을 닫는다. 보내는 중 파일이 줄면 멈추고, 늘면 처음 크기까지만 보낸다. | 이미 `200 OK`를 보내서 상태 코드를 바꿀 수 없어요. `Content-Length`와 실제 본문이 달라지지 않게 해요. |
| 3-10 | `send_response()`에서 헤더 부분을 `send_header(fd, status, content_type, content_length)`로 뗀다. `send_response()`는 안에서 `send_header()`를 부른다. | 파일은 본문을 조각내서 보내니 헤더만 먼저 보낼 함수가 필요해요. 응답 헤더를 만드는 곳은 여전히 한 군데예요. |
| 3-11 | MIME 표: `html`/`htm` → `text/html; charset=utf-8`, `css` → `text/css; charset=utf-8`, `js` → `text/javascript; charset=utf-8`, `txt` → `text/plain; charset=utf-8`, `json` → `application/json`, `png` → `image/png`, `jpg`/`jpeg` → `image/jpeg`, `gif` → `image/gif`, `svg` → `image/svg+xml`, `ico` → `image/x-icon`, 그 밖 → `application/octet-stream`. | 텍스트에 `charset=utf-8`을 붙여 한글이 깨지지 않아요. `.js`는 RFC 9239 표준을 따라요. 모르는 건 "그냥 바이트"로 보내야 브라우저가 글자로 보여 주지 않아요. |
| 3-12 | 확장자는 마지막 `/` 뒤 이름에서 마지막 `.` 뒤이고, 대소문자를 구분하지 않는다. 이름이 `.`으로 시작하면 확장자가 없다. MIME은 `realpath()`로 풀기 전 경로로 정한다. | `PHOTO.JPG`도 이미지예요. `/v1.2/readme`처럼 디렉터리 이름의 `.`은 확장자가 아니에요. 심볼릭 링크라도 요청한 이름 기준으로 타입을 정해요. |
| 3-13 | MIME은 `mime.c`/`mime.h`의 `mime_type(path)` 하나로 하고, 안에 `{확장자, 타입}` 배열을 둔다. `tests/test_mime.c`로 단위 테스트한다. | 새 타입은 표에 한 줄만 추가해요. MIME 매핑 단위 테스트는 과제 요구사항이에요. (DESIGN.md 제안과 같음) |
| 3-14 | `make test`는 `tests/test_X.c`와 `src/X.c`를 짝지어 빌드하고, 하나가 실패해도 모든 테스트를 돌린 뒤 실패가 있으면 종료 코드 1로 끝낸다. | 테스트 파일이 늘어도 `TESTS` 목록에 이름만 추가하면 돼요. 한 번에 모든 실패를 볼 수 있어요. |
| 3-15 | 에러 판정: `ENOENT`/`ENOTDIR` → 404, `EACCES` → 403, 루트 밖 → 403, 일반 파일도 디렉터리도 아님 → 403, 경로가 `PATH_MAX`를 넘음 → 414, 그 밖의 실패 → 500. | 404는 없음, 403은 있지만 줄 수 없음, 500은 서버 쪽 문제예요. 414는 "URI가 너무 길다"는 전용 코드라 400보다 원인이 분명해요. |
| 3-16 | 파일 서빙은 `GET`만 한다. `POST`는 Step 4까지 Step 2의 고정 응답을 그대로 보낸다. | `POST`가 무엇을 할지는 본문을 읽는 Step 4에서 정해요. |
| 3-17 | 정적 파일 처리는 `static.c`/`static.h`의 `serve_static(client_fd, root, &req)`로 한다. 응답까지 보내고 상태 코드를 돌려줘 로그에 쓴다. `server_run()`은 루트 경로를 받는다. | `server.c`는 흐름만 담고, 라우팅이 붙는 Step 6에서 정적 처리기를 그대로 부를 수 있어요. |
| 3-18 | 컴파일 옵션에 `-D_XOPEN_SOURCE=700`을 더한다. | glibc는 `realpath()`를 X/Open 기능으로 분류해서 이게 없으면 선언하지 않아요. POSIX.1-2008의 상위 집합이에요. |

## 만드는 파일과 함수

| 파일 | 내용 |
| ---- | ---- |
| `src/mime.h`, `src/mime.c` | `mime_type(path)`와 MIME 표 |
| `src/static.h`, `src/static.c` | `serve_static()`: 경로 만들기, 탈출 검사, 파일 전송 |
| `src/response.h`, `src/response.c` | `send_header()` 추가, `status_text()`에 414 추가 |
| `src/server.h`, `src/server.c` | `server_run(listen_fd, root)`. `GET`이면 `serve_static()`을 불러요 |
| `src/main.c` | 루트의 `realpath()`를 구해 `server_run()`에 넘겨요 |
| `tests/test_mime.c` | MIME 단위 테스트 |
| `Makefile` | 테스트 규칙을 `test_X.c` ↔ `X.c` 짝으로 일반화, `-D_XOPEN_SOURCE=700` 추가 |

`static.c`의 함수예요.

| 함수 | 하는 일 |
| ---- | ------- |
| `serve_static(client_fd, root, &req)` | ① 쿼리를 떼고 루트 + URI로 경로를 만들어요 → `resolve_file()` → ⑤ `open()` → ⑥ `send_file()`. 실패하면 에러 응답을 보내요. 보낸 상태 코드를 돌려줘요 |
| `resolve_file(root, path, real)` | ② `realpath()` → ③ `is_inside()` → ④ `stat()`. 디렉터리면 `index.html`을 붙여 한 번 더 해요 |
| `is_inside(root, real)` | `real`이 루트와 같거나 루트 + `/`로 시작하면 1이에요 |
| `status_from_errno(err)` | `ENOENT`/`ENOTDIR` → 404, `EACCES` → 403, 그 밖 → 500 |
| `send_file(client_fd, file_fd, type, size)` | `send_header()` 뒤에 64KB씩 `read()` → `write_all()`을 `size`만큼 반복해요 |

## 완료 테스트

모든 명령은 컨테이너 안에서 실행해요. 프롬프트가 `dev@xxxx:/app$`인지 먼저 확인해요. 이전 Step의 서버가 켜져 있으면 Ctrl+C로 끄고 다시 켜요 (켜져 있으면 `bind: Address already in use`가 나와요).

```sh
# 터미널 A
make && ./webserver -p 8080 -r ./www
```

**1. 단위 테스트**

```sh
# 터미널 B
make test
# → test_request: 49 checks, 0 failures
# → test_mime: 24 checks, 0 failures
```

**2. 브라우저** (완료 기준, Mac에서)

Mac 브라우저로 `http://localhost:8080/`을 열어요.

- 제목이 파란색이고 배경이 연한 회색이에요 → `style.css`가 `text/css`로 왔어요.
- "JS가 정상으로 실행됐어요."가 초록색 굵은 글씨로 보여요 → `app.js`가 `text/javascript`로 왔어요.
- PNG, JPG 그림이 둘 다 보여요.

**3. 파일 종류별 응답**

```sh
for f in index.html style.css app.js image.png photo.jpg large.bin; do
  curl -s -o /dev/null -w "$f %{http_code} %{content_type} %{size_download}\n" http://localhost:8080/$f
done
# → 모두 200, 타입은 3-11 표대로, 크기는 ls -l www 와 같아요
```

**4. 바이너리가 그대로 오는지**

```sh
for f in image.png photo.jpg large.bin; do curl -s http://localhost:8080/$f | cmp - www/$f && echo "$f same"; done
curl -s http://localhost:8080/ | cmp - www/index.html && echo "/ == index.html"
```

**5. 에러** (완료 기준 포함)

```sh
curl -i http://localhost:8080/nope.html                           # → 404 (완료 기준)
curl -i http://localhost:8080/secret.txt                          # → 403 (완료 기준)
curl -i --path-as-is http://localhost:8080/../../etc/passwd       # → 403 (완료 기준)
curl -i http://localhost:8080/index.html/x                        # → 404 (ENOTDIR)
curl -i --path-as-is http://localhost:8080/%2e%2e/etc/passwd      # → 404 (디코딩 안 함)
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/$(head -c 5000 /dev/zero | tr '\0' a)   # → 414
curl -s -o /dev/null -w "%{http_code}\n" "http://localhost:8080/style.css?v=2"                        # → 200
```

`secret.txt`가 200이면 `make www`를 다시 하거나, 컨테이너가 root로 돌고 있지 않은지 확인해요 (결정 0-13).

**6. 심볼릭 링크 탈출** (realpath를 고른 이유)

```sh
ln -s /etc/passwd www/escape
curl -i http://localhost:8080/escape     # → 403
rm www/escape
```

URI에는 `..`이 없지만 `realpath()`가 링크를 풀어서 루트 밖인 걸 알아내요.

**7. 메모리 검사**

```sh
# 터미널 A: 서버를 끄고 디버그 빌드로 다시 실행
make debug && ./webserver-debug -p 8080 -r ./www
# 터미널 B: 테스트 3~6을 다시 실행
# → 터미널 A에 ASan 에러가 나오지 않아요
```
