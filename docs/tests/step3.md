# Step 3 테스트

이 문서를 읽으면 Step 3을 어떤 순서로 테스트하고, 결과를 어디에 기록하는지 알 수 있어요. 요구사항은 Step 3 요구사항(S3-01 ~ S3-16), 설계는 [Step 3 설계](../design/step3.md)에 있어요.

## 1. 테스트 범위

| 종류 | 확인하는 것 | 요구사항 |
| --- | --- | --- |
| 단위 테스트 (경로·MIME) | `file.c` 함수가 확장자, `..` 조각, 요청 경로를 맞게 다루는지 | S3-01 ~ S3-04, S3-08 ~ S3-13 |
| 단위 테스트 (연결) | `handle_conn()`이 파일을 맞는 헤더와 함께 바이트 그대로 보내고, 오류에 맞는 상태 코드를 보내는지 | S3-01, S3-02, S3-04 ~ S3-08, S3-10, S3-11, S3-13, S3-14 |
| 기능 테스트 | 서버를 띄우고 `curl`과 브라우저로 요청했을 때 | S3-01 ~ S3-14 |
| 코드 리뷰 | 테스트로 일으키기 어려운 오류 경로 | S3-07, S3-15, S3-16 |

Step 2의 200 응답(`Content-Length: 0`)은 Step 3부터 파일 응답으로 바뀌어요. 그래서 Step 2의 UT-40은 응답 모양만 보도록 바꾸고, 내용은 아래 UT-33에서 봐요.

## 2. 준비

- Dev Container 터미널에서 저장소 루트로 이동해요.
- `make www`로 `www/large.bin`과 읽기 권한이 없는 `www/secret.txt`를 만들어요.
- 단위 테스트는 `/tmp` 아래에 테스트용 루트를 만들고 끝나면 지워요. 저장소의 `www/`는 건드리지 않아요.
- root 사용자는 권한 000 파일도 읽을 수 있어서, 권한 테스트(UT-37, FT-12)는 일반 사용자로 해요.

## 3. 단위 테스트: 경로·MIME (`tests/test_file.c`)

### mime_type

| 번호 | 넣는 것 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-01 | `/index.html` | `text/html; charset=utf-8` | S3-08 |
| UT-02 | `/style.css` | `text/css; charset=utf-8` | S3-08 |
| UT-03 | `/app.js` | `text/javascript; charset=utf-8` | S3-08 |
| UT-04 | `/image.png` | `image/png` | S3-08 |
| UT-05 | `/photo.jpg` | `image/jpeg` | S3-08 |
| UT-06 | `/photo.jpeg` | `image/jpeg` | S3-08 |
| UT-07 | `/PHOTO.JPG` | `image/jpeg` | S3-09 |
| UT-08 | `/large.bin` | `application/octet-stream` | S3-10 |
| UT-09 | `/README` (확장자 없음) | `application/octet-stream` | S3-10 |
| UT-10 | `/a.d/file` (폴더 이름에만 `.`) | `application/octet-stream` | S3-10 |
| UT-11 | `/x.tar.png` | `image/png` (마지막 `.` 기준) | S3-08 |

### has_dot_segment

| 번호 | 넣는 것 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-12 | `/../../etc/passwd` | `1` | S3-11 |
| UT-13 | `/a/..` | `1` | S3-11 |
| UT-14 | `/a/../b` | `1` | S3-11 |
| UT-15 | `/..` | `1` | S3-11 |
| UT-16 | `/a..b.html` | `0` | S3-11 |
| UT-17 | `/..hidden` | `0` | S3-11 |
| UT-18 | `/./style.css` | `0` | S3-11 |
| UT-19 | `/style.css` | `0` | S3-11 |

### resolve_path

`mkdtemp()`로 만든 임시 폴더 안에 아래 구조를 만들고, `www`의 실제 위치를 `root_real`로 넘겨요.

```
<base>/www/index.html, style.css, secret.txt(권한 000)
<base>/www/sub/index.html
<base>/www/empty/            (index.html 없음)
<base>/www/link  -> /etc     (루트 밖을 가리키는 링크)
<base>/www/link2 -> ../www2  (이름이 www로 시작하는 옆 폴더)
<base>/www2/x.html
```

| 번호 | 넣는 것 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-20 | `/style.css` | `0`, `out` = 루트`/style.css` | S3-01 |
| UT-21 | `/` | `0`, `out` = 루트`/index.html` | S3-02 |
| UT-22 | `/sub/` | `0`, `out` = 루트`/sub/index.html` | S3-02 |
| UT-23 | `/sub` (끝 `/` 없음) | `0`, `out` = 루트`/sub/index.html` | S3-02 |
| UT-24 | `/style.css?v=3` | `0`, `out` = 루트`/style.css`, 넘긴 경로가 `/style.css`로 잘림 | S3-04 |
| UT-25 | `/empty/` | `403` | S3-03 |
| UT-26 | `/nope.html` | `404` | S3-13 |
| UT-27 | `/style.css/x` (파일 아래 경로) | `404` | S3-13 |
| UT-28 | `/../www2/x.html` | `403` | S3-11 |
| UT-29 | `/link/passwd` | `403` | S3-12 |
| UT-30 | `/link2/x.html` | `403` (`www2`는 `www/`로 시작하지 않음) | S3-12 |
| UT-31 | `/secret.txt` | `0` (위치는 루트 안. 권한은 파일을 열 때 봄) | S3-14 |

## 4. 단위 테스트: 연결 (`tests/test_server.c`)

`mkdtemp()`로 만든 임시 폴더에 `index.html`, `style.css`, `image.png`(0 ~ 255 바이트 256개), `large.bin`(1MB), `secret.txt`(권한 000)를 만들어 루트로 써요. 자식 프로세스가 `handle_conn(c, root_real)`을 돌리고, 부모가 클라이언트로서 요청을 쓰고 응답을 끝까지 읽어요. 응답이 커도 부모가 바로 읽어 주니 서로 기다리다 멈추지 않아요.

| 번호 | 요청 | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-32 | `GET /style.css` | 리턴 `0`, `200 OK`, `Content-Type: text/css; charset=utf-8`, `Connection: close`, 본문이 파일과 같고 `Content-Length`가 파일 크기 | S3-01, S3-05, S3-08 |
| UT-33 | `GET /` | 응답 전체가 정확히 `200 OK` + `text/html; charset=utf-8` + `Content-Length` + `Connection: close` + `index.html` 내용 | S3-02, S3-05 |
| UT-34 | `GET /image.png` | `Content-Type: image/png`, 0 ~ 255 바이트(`\0` 포함)가 그대로 | S3-06, S3-08 |
| UT-35 | `GET /large.bin` | `Content-Type: application/octet-stream`, 1MB가 그대로 | S3-07, S3-10 |
| UT-36 | `GET /nope.html` | `404 Not Found`, 본문 `404 Not Found\n` | S3-13 |
| UT-37 | `GET /secret.txt` (일반 사용자만) | `403 Forbidden`, 본문 `403 Forbidden\n` | S3-14 |
| UT-38 | `GET /../../etc/passwd` | `403 Forbidden` | S3-11 |
| UT-39 | `GET /style.css?v=3` | `200 OK`, 본문이 `style.css`와 같음 | S3-04 |

**통과 기준**: `make test`가 `test_config`, `test_http`, `test_file`, `test_server`에서 실패 없이 끝나고, `make clean && make test-debug`로 ASan·UBSan을 켜고 돌려도 오류가 없다.

## 5. 기능 테스트

터미널 1에서 `make && make www && ./webserver -p 8080 -r ./www`로 서버를 띄운 상태에서 해요.

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| FT-01 | S3-01 | `curl -s http://localhost:8080/style.css \| cmp - www/style.css && echo same` | `same` |
| FT-02 | S3-02 | `curl -s http://localhost:8080/ \| cmp - www/index.html && echo same` | `same` |
| FT-03 | S3-03 | `mkdir -p www/empty`<br>`curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/empty/`<br>`rmdir www/empty` | `403` |
| FT-04 | S3-04 | `curl -s 'http://localhost:8080/style.css?v=3' \| cmp - www/style.css && echo same` | `same` |
| FT-05 | S3-05 | `curl -sI http://localhost:8080/style.css \| grep -i content-length`<br>`stat -c %s www/style.css` | 두 숫자가 같다 |
| FT-06 | S3-06 | `curl -s http://localhost:8080/image.png \| cmp - www/image.png && echo same`<br>`curl -s http://localhost:8080/photo.jpg \| cmp - www/photo.jpg && echo same` | `same` 두 번 |
| FT-07 | S3-07 | `curl -s http://localhost:8080/large.bin \| cmp - www/large.bin && echo same` | `same` |
| FT-08 | S3-08 | `for f in index.html style.css app.js image.png photo.jpg; do curl -sI http://localhost:8080/$f \| grep -i content-type; done` | `text/html; charset=utf-8`, `text/css; charset=utf-8`, `text/javascript; charset=utf-8`, `image/png`, `image/jpeg` |
| FT-09 | S3-10 | `curl -sI http://localhost:8080/large.bin \| grep -i content-type` | `Content-Type: application/octet-stream` |
| FT-10 | S3-11 | `curl -s -o /dev/null -w '%{http_code}\n' --path-as-is http://localhost:8080/../../etc/passwd` | `403` |
| FT-11 | S3-12 | `ln -s /etc www/link`<br>`curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/link/passwd`<br>`rm www/link` | `403` |
| FT-12 | S3-14 | `curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/secret.txt` | `403` |
| FT-13 | S3-13 | `curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/nope.html` | `404` |
| FT-14 | S3-01 ~ S3-08 | 브라우저로 `http://localhost:8080/` 열기 | `index.html`이 열리고 CSS·JS·PNG·JPG가 모두 보인다. 개발자 도구 네트워크 탭에 실패한 요청이 없다 |
| FT-15 | 설계 | 서버를 `make debug && ./webserver -p 8080 -r ./www`로 다시 띄우고 FT-01 ~ FT-13을 보낸 뒤 Ctrl+C | ASan 오류 보고가 없다 |

## 6. 코드 리뷰

`src/file.c`와 `src/server.c`를 보고 확인해요.

| 번호 | 요구사항 | 확인할 것 |
| --- | --- | --- |
| CR-01 | S3-07 | `send_file()`은 파일 전체를 `malloc`하지 않고, 8KB 버퍼로 `read()`·`write_all()`을 반복한다 |
| CR-02 | S3-15 | `open()`이 `ENOENT`·`EACCES`가 아닌 이유로 실패하거나 `fstat()`이 실패하면 `500`을 돌려준다 |
| CR-03 | S3-16 | 헤더를 보낸 뒤 `read()`나 `write_all()`이 실패하면 `-1`을 돌려주고, `handle_conn()`은 오류 응답 없이 닫는다 |
| CR-04 | 설계 | `send_file()`에서 연 파일은 모든 리턴 경로에서 `close()`된다 |
| CR-05 | S3-12 | `is_inside()`는 루트 뒤에 `\0`이나 `/`가 오는지까지 본다 |
| CR-06 | S3-02 | 디렉터리일 때 붙인 `index.html`도 `realpath()`와 루트 안인지 검사를 다시 거친다 |
| CR-07 | 설계 | `root_real`은 `main()`에서 한 번만 구하고, 요청마다 다시 구하지 않는다 |

## 7. 결과 기록

테스트를 돌린 뒤 이 표를 채워요. 실패하면 비고에 무엇이 나왔는지 적어요.

| 날짜 | 커밋 | 단위 (UT) | 기능 (FT) | 코드 리뷰 (CR) | 비고 |
| --- | --- | --- | --- | --- | --- |
| | | / 39 | / 15 | / 7 | |
