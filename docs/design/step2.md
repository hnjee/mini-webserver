# Step 2 설계: HTTP 요청 파싱과 고정 응답

이 문서를 읽으면 Step 2 서버가 요청 줄과 헤더를 어떻게 모으고 나누는지, 어떤 응답을 보내는지 알 수 있어요. 함수별 동작 순서를 그대로 따라 `src/http.c`와 `src/server.c`를 구현할 수 있어요.

요구사항은 Step 2 요구사항(S2-01 ~ S2-23)을 기준으로 해요.

## 자료구조

### 연결 구조체: struct conn

연결 하나에 하나씩 `malloc`으로 만들어요. 헤더 버퍼가 이 안에 고정 크기로 들어 있어요. (S2-01, S2-05)

```c
#define HEADER_BUF_SIZE 8192

struct conn {
    int fd;                          /* 클라이언트 소켓 번호 */
    char buf[HEADER_BUF_SIZE];       /* 요청 줄과 헤더를 모으는 헤더 버퍼 */
    size_t len;                      /* buf에 지금까지 모은 바이트 수 */
    struct http_request req;         /* 파싱 결과 */
};
```

크기는 대부분 헤더 버퍼예요.

| 항목    | 크기                                             |
| ------- | ------------------------------------------------ |
| `fd`  | 4바이트                                          |
| `buf` | 8192바이트                                       |
| `len` | 8바이트                                          |
| `req` | 약 540바이트 (포인터 3개 + 헤더 32쌍 + 개수)     |
| 합계    | 약 8.7KB                                         |

### 파싱 결과: struct http_request

파싱 결과는 문자열을 따로 복사하지 않고, 헤더 버퍼 안을 가리키는 포인터로 담아요. 나눌 자리(공백, `:`, `\r`)에 `\0`을 써서 각 조각을 C 문자열로 만들어요.

```c
#define HTTP_MAX_HEADERS 32

struct http_header {
    char *name;                      /* 예: "Host" */
    char *value;                     /* 예: "localhost" (앞뒤 공백 지움) */
};

struct http_request {
    char *method;                    /* 예: "GET" */
    char *path;                      /* 예: "/index.html" */
    char *version;                   /* 예: "HTTP/1.1" */
    struct http_header headers[HTTP_MAX_HEADERS];
    int header_count;
};
```

`GET / HTTP/1.1\r\nHost: x\r\n\r\n`을 파싱하면 헤더 버퍼는 이렇게 바뀌어요.

```
파싱 전: G E T ␠ / ␠ H T T P / 1 . 1 \r \n H o s t : ␠ x \r \n \r \n
파싱 후: G E T \0 / \0 H T T P / 1 . 1 \0 \n H o s t \0 ␠ x \0 \n \r \n
         ↑method ↑path ↑version             ↑name      ↑value
```

## 전체 흐름

```
server_run()
  accept → conn 만들기(malloc) → handle_conn() → close → conn 해제(free) → 다시 accept

handle_conn()
  read 반복              헤더의 끝(\r\n\r\n)까지 헤더 버퍼에 이어 붙여 모아요
  http_parse_request()   요청 줄 → 헤더 순서로 나누고 검사해요
  send_response()        200 또는 오류 응답을 write_all()로 보내요
  drain()                쓰기를 먼저 닫고(FIN), 남은 데이터를 끊길 때까지 읽어 버려요
```

- Step 1의 에코(`serve_client()`)는 없어지고 `handle_conn()`이 그 자리에 들어가요.
- 파싱은 `http.c`가 맡아요. 소켓을 모르고 버퍼만 받아서, 단위 테스트로 바로 확인할 수 있어요.
- 연결 하나에 요청 하나, 응답 하나예요. 응답을 보내면 닫아요.

## 헤더의 끝 찾기: http_find_header_end

`buf[0..len)`에서 `\r\n\r\n`을 찾아, 있으면 끝 바로 다음 위치를, 없으면 `0`을 돌려줘요. (S2-02)

헤더의 끝이 두 번의 `read()`에 걸쳐 와도 찾을 수 있게, `read()` 할 때마다 버퍼 처음부터 다시 찾아요. 버퍼가 8KB라 다시 찾아도 부담이 없어요.

```c
size_t http_find_header_end(const char *buf, size_t len)
{
    for (size_t i = 3; i < len; i++) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' &&
            buf[i - 1] == '\r' && buf[i] == '\n')
            return i + 1;
    }
    return 0;
}
```

## 요청 파싱: http_parse_request

헤더의 끝까지 들어 있는 버퍼를 받아 `req`를 채워요. 검사 순서는 S2-23을 따라요.

| 돌려주는 값                 | 뜻                                     |
| --------------------------- | -------------------------------------- |
| `0`                       | 올바른 요청이에요. `req`를 다 채웠어요. |
| `400`, `431`, `501`, `505` | 그 상태 코드로 응답해야 해요.          |

동작 순서

1. 헤더 영역에 `\0`이 있으면 `400`이에요. C 문자열 함수가 `\0`을 끝으로 봐서 그 뒤를 못 보기 때문이에요.
2. 마지막 `\r\n\r\n`의 첫 `\r`에 `\0`을 써서, 요청 줄과 헤더 전체를 C 문자열 하나로 만들어요.
3. 줄 수를 세어 헤더 줄이 32개를 넘으면 `431`을 돌려줘요. (S2-03)
4. 첫 줄을 `parse_request_line()`으로 나눠요. `400` → `501` → `505` 순서로 검사해요. (S2-06 ~ S2-10)
5. 나머지 줄을 `parse_headers()`로 나눠요. 형식이 틀리면 `400`이에요. (S2-11, S2-13)
6. `HTTP/1.1`인데 `Host`가 없으면 `400`이에요. (S2-14)
7. `0`을 돌려줘요.

```c
int http_parse_request(char *buf, size_t len, struct http_request *req)
{
    if (memchr(buf, '\0', len) != NULL)
        return 400;                              /* 요청 안에 \0 */

    buf[len - 4] = '\0';                         /* 헤더 영역을 C 문자열로 */

    int lines = 1;
    for (char *p = buf; (p = strstr(p, "\r\n")) != NULL; p += 2)
        lines++;
    if (lines - 1 > HTTP_MAX_HEADERS)
        return 431;

    char *rest = strstr(buf, "\r\n");            /* 요청 줄 끝 */
    if (rest != NULL) {
        *rest = '\0';
        rest += 2;
    }

    int status = parse_request_line(buf, req);
    if (status != 0)
        return status;

    status = parse_headers(rest, req);
    if (status != 0)
        return status;

    if (strcmp(req->version, "HTTP/1.1") == 0 && http_find_header(req, "Host") == NULL)
        return 400;
    return 0;
}
```

### 요청 줄: parse_request_line

공백 하나로 나뉜 세 부분이어야 해요. 대소문자를 구분해서 `get`은 `501`이에요.

```c
static int parse_request_line(char *line, struct http_request *req)
{
    char *sp1 = strchr(line, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (sp2 == NULL || strchr(sp2 + 1, ' ') != NULL)
        return 400;                              /* 세 부분이 아님 */

    *sp1 = '\0';
    *sp2 = '\0';
    req->method = line;
    req->path = sp1 + 1;
    req->version = sp2 + 1;

    if (req->method[0] == '\0' || req->path[0] != '/' || !is_version_shape(req->version))
        return 400;
    if (strcmp(req->method, "GET") != 0)
        return 501;
    if (strcmp(req->version, "HTTP/1.0") != 0 && strcmp(req->version, "HTTP/1.1") != 0)
        return 505;
    return 0;
}

/* "HTTP/숫자.숫자" 모양인지 */
static int is_version_shape(const char *v)
{
    return strlen(v) == 8 && strncmp(v, "HTTP/", 5) == 0 &&
           isdigit((unsigned char)v[5]) && v[6] == '.' && isdigit((unsigned char)v[7]);
}
```

### 헤더: parse_headers

한 줄씩 첫 번째 `:`에서 나누고 값의 앞뒤 공백과 탭을 지워요.

```c
static int parse_headers(char *p, struct http_request *req)
{
    req->header_count = 0;
    while (p != NULL && *p != '\0') {
        char *next = strstr(p, "\r\n");
        if (next != NULL) {
            *next = '\0';
            next += 2;
        }

        char *colon = strchr(p, ':');
        if (colon == NULL || colon == p || colon[-1] == ' ' || colon[-1] == '\t')
            return 400;                          /* : 없음, 이름 비었음, : 앞 공백 */
        *colon = '\0';

        struct http_header *h = &req->headers[req->header_count++];
        h->name = p;
        h->value = trim(colon + 1);
        p = next;
    }
    return 0;
}

/* 앞뒤 공백과 탭을 지운 시작 위치를 돌려줘요 */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return s;
}
```

### 헤더 찾기: http_find_header

이름을 대소문자 구분 없이 비교해요. (S2-12)

```c
const char *http_find_header(const struct http_request *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}
```

## 응답 보내기: send_response, write_all

상태 코드를 받아 응답 전체를 만든 뒤 `write_all()`로 보내요. (S2-15 ~ S2-22)

- `200`: 본문 없이 `Content-Length: 0`을 보내요.
- 오류: `Content-Type: text/plain`과 본문 한 줄을 보내요. 본문은 `상태 코드 문구\n`이에요. (예: `501 Not Implemented\n`)
- 상태 줄 버전은 요청과 관계없이 `HTTP/1.1`이에요.

```c
static int send_response(int fd, int status)
{
    char res[256];
    int n;

    if (status == 200) {
        n = snprintf(res, sizeof(res),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n"
                     "\r\n");
    } else {
        char body[64];
        int body_len = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
        n = snprintf(res, sizeof(res),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     status, http_reason(status), body_len, body);
    }
    return write_all(fd, res, (size_t)n);
}
```

```c
int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(fd, buf + sent, len - sent);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            perror("write");
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}
```

```c
const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}
```

## 닫기 전 비우기: drain

받기 버퍼에 안 읽은 데이터가 남은 채 `close()`하면 커널이 FIN 대신 RST를 보내요. 그러면 클라이언트 커널이 아직 안 읽은 응답까지 버려서, 클라이언트가 응답을 못 받을 수 있어요. 헤더가 8KB를 넘는 요청(S2-03)이나 헤더의 끝 뒤에 데이터가 더 붙은 요청이 그래요.

동작 순서

1. `shutdown(fd, SHUT_WR)`로 쓰기만 닫아요. 커널이 FIN을 보내고, 클라이언트는 응답을 끝까지 받아요.
2. `read()`가 `0`(클라이언트가 끊음)이나 실패를 돌려줄 때까지 읽어서 버려요. 단, 16KB까지만 읽어요.
3. 받기 버퍼가 비었으니 이후 `close()`에서 RST가 나가지 않아요. 16KB를 넘게 더 보내는 클라이언트는 그냥 닫아서 RST로 끊겨도 괜찮아요.

클라이언트가 끊지 않으면 2번에서 계속 기다려요. Step 4 타임아웃에서 풀어요.

```c
#define DRAIN_LIMIT (16 * 1024)

static void drain(int fd)
{
    char tmp[4096];
    size_t total = 0;

    if (shutdown(fd, SHUT_WR) < 0) {
        perror("shutdown");
        return;
    }
    while (total < DRAIN_LIMIT) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n == 0)
            return;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        total += (size_t)n;
    }
}
```

## 연결 하나 처리: handle_conn

동작 순서

1. `read()`를 반복해 헤더 버퍼에 이어 붙이다가 헤더의 끝을 찾으면 멈춰요. (S2-02)
   - 헤더의 끝 전에 버퍼가 가득 차면 `431`이에요. (S2-03)
   - 헤더의 끝 전에 `read()`가 `0`이거나 실패하면 응답하지 않고 돌아가요. (S2-04)
2. `431`이 아니면 `http_parse_request()`로 파싱해요.
3. 결과가 `0`이면 `200`으로 바꿔요.
4. `send_response()`로 보내요.
5. `drain()`으로 받기 버퍼를 비워요.

```c
int handle_conn(struct conn *c)
{
    size_t end;
    int status = 0;

    while ((end = http_find_header_end(c->buf, c->len)) == 0) {
        if (c->len == sizeof(c->buf)) {          /* 헤더의 끝 전에 버퍼가 참 */
            status = 431;
            break;
        }
        ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
        if (n == 0)
            return -1;                           /* 헤더의 끝 전에 끊김: 응답 없음 */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return -1;
        }
        c->len += (size_t)n;
    }

    if (status == 0)
        status = http_parse_request(c->buf, end, &c->req);
    if (status == 0)
        status = 200;
    if (send_response(c->fd, status) < 0)
        return -1;
    drain(c->fd);
    return 0;
}
```

## server_run 변경

`serve_client()` 대신 연결 구조체를 만들어 `handle_conn()`을 불러요. 닫기와 해제는 여기서 한 번만 해요. (S2-05, S2-18, S2-21)

```c
void server_run(int listen_fd)
{
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }

        struct conn *c = malloc(sizeof(*c));
        if (c == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }
        c->fd = client_fd;
        c->len = 0;

        handle_conn(c);

        if (close(c->fd) < 0)
            perror("close");
        free(c);
    }
}
```

## 시나리오별 실행 순서

### 올바른 GET (`curl http://localhost:8080/`)

1. `accept()`가 `4`를 돌려주고 `conn`을 만들어요.
2. `read()` 한 번에 요청 전체가 들어와요. 헤더의 끝을 찾아요.
3. 요청 줄은 `GET`, `/`, `HTTP/1.1`이고, 헤더는 `Host`, `User-Agent`, `Accept` 3개예요.
4. `200 OK`, `Content-Length: 0`을 보내요. (S2-07, S2-15, S2-16)
5. `close(4)`, `free(c)` 후 다시 `accept()`로 돌아가요.

### 요청이 나뉘어 옴 (S2-02 확인 명령어)

1. 첫 `read()`에 `GET / HTTP/1.1\r\n`만 들어와요. 헤더의 끝이 없어서 다시 `read()`에서 잠들어요.
2. 1초 뒤 `Host: x\r\n\r\n`이 들어와 버퍼 뒤에 붙어요. 이제 헤더의 끝을 찾아요.
3. 파싱하고 `200`을 보내요.

### 헤더가 8KB를 넘음 (S2-03 확인 명령어)

1. `read()`를 반복해 버퍼 8192바이트가 다 차도록 헤더의 끝이 없어요.
2. `status`가 `431`이 되어 읽기를 멈추고, 파싱 없이 `431` 응답을 보내요.
3. `drain()`이 FIN을 보내고, 받기 버퍼에 남은 808바이트를 읽어 버려요. `nc -N`이 끊으면 `read()`가 `0`이에요.
4. `close()`해도 RST가 나가지 않아 클라이언트는 431을 받아요.

### 헤더의 끝 전에 끊김 (S2-04 확인 명령어)

1. `GET / HTTP/1.1\r\n`을 받은 뒤 다음 `read()`가 `0`이에요.
2. `handle_conn()`이 응답 없이 `-1`을 돌려주고, `server_run()`이 닫아요.

### 지원하지 않는 메서드 (`BREW / HTTP/2.0`)

1. 요청 줄 형식은 맞아요.
2. 메서드 검사에서 먼저 걸려 `501`을 보내요. 버전(505)까지 가지 않아요. (S2-23)

## 파일 구성

| 파일                  | 내용                                                               | Step 2 변경 |
| --------------------- | ------------------------------------------------------------------ | ----------- |
| `include/http.h`    | `http_request` 구조체, 파싱 함수 선언                             | 새로 만듦   |
| `src/http.c`        | 헤더의 끝 찾기, 요청 줄·헤더 파싱, 상태 문구                      | 새로 만듦   |
| `include/server.h`  | `conn` 구조체, `handle_conn`, `write_all` 추가, `serve_client` 삭제 | 수정        |
| `src/server.c`      | `send_response`, `drain`, `handle_conn`, `server_run` 변경 | 수정        |
| `tests/test_http.c` | 파싱 단위 테스트                                                   | 새로 만듦   |
| `tests/test_server.c` | 에코 테스트를 `handle_conn` 테스트로 바꿈                         | 수정        |

```c
/* include/http.h */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_MAX_HEADERS 32

struct http_header {
    char *name;
    char *value;
};

struct http_request {
    char *method;
    char *path;
    char *version;
    struct http_header headers[HTTP_MAX_HEADERS];
    int header_count;
};

/* buf[0..len)에서 헤더의 끝(\r\n\r\n)을 찾아요. 있으면 끝 바로 다음 위치, 없으면 0 */
size_t http_find_header_end(const char *buf, size_t len);

/* 헤더의 끝까지 든 buf를 파싱해 req를 채워요. buf 안에 '\0'을 써요. 성공 0, 실패하면 응답할 상태 코드 */
int http_parse_request(char *buf, size_t len, struct http_request *req);

/* 헤더 이름을 대소문자 구분 없이 찾아 값을 돌려줘요. 없으면 NULL */
const char *http_find_header(const struct http_request *req, const char *name);

/* 상태 코드의 문구 (예: 501 → "Not Implemented") */
const char *http_reason(int status);

#endif
```

`http.c`에 필요한 헤더는 `<ctype.h>`, `<string.h>`, `<strings.h>`(`strcasecmp`)예요. `server.c`에는 `<stdlib.h>`와 `"http.h"`를 더해요.

## 테스트 방법

| 대상                     | 방법                                                                                                    | 근거                   |
| ------------------------ | ------------------------------------------------------------------------------------------------------- | ---------------------- |
| `http_find_header_end()` | 끝이 있는 버퍼, 없는 버퍼, `\r\n\r`까지만 있는 버퍼를 넣어 위치를 봐요.                                | S2-02                  |
| `http_parse_request()`   | 문자열을 배열에 복사해 넣고 `req` 값과 돌려준 상태 코드를 봐요. 틀린 경우마다 한 줄씩 만들어요. 요청 줄이나 헤더 중간에 `\0`이 든 경우도 400인지 봐요. | S2-06 ~ S2-14, S2-23   |
| `handle_conn()`          | `socketpair()`로 한쪽에 요청을 쓰고, 다른 쪽에서 응답을 읽어 상태 줄·헤더·본문을 봐요.                 | S2-15 ~ S2-20          |
| 서버 전체                | 요구사항 표의 확인 명령어를 그대로 실행하고, `make debug`로 띄워 ASan 누수 보고가 없는지 봐요.           | S2-01 ~ S2-23          |
