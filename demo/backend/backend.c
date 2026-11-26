/* 시연 전용 소형 백엔드. 제품 코드가 아니다.
 * 사용법: backend <포트> <이름>   예) backend 8001 A
 * GET /menu           → 데모 메뉴 3개(두 인스턴스 동일) + 이 인스턴스 이름 + 이 요청에서 받은 머리(화이트리스트)
 * GET /inspect        → 이 요청에서 받은 요청 줄과 머리(화이트리스트)만
 * GET /identity.json  → 이 인스턴스의 이름·포트·받은 경로
 * GET /slow?ms=N      → N ms(0~10000) 기다린 뒤 응답. 5초를 넘기면 mini-webserver가 504를 낸다.
 * 그 밖               → 404
 * 연결마다 fork 하므로 느린 요청 중에도 다른 요청에 답한다. 응답 뒤 연결을 닫는다.
 * Cookie·Authorization 등 화이트리스트 밖의 머리는 이름도 값도 돌려주지 않고 개수만 센다.
 * 돌려주는 값은 JSON escape한 원문이다. 생략·대체한 것은 omitted·replaced_bytes로 표시한다. */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int port;
static const char *name;

static const char *const allowed[] = {
    "Host", "Connection", "Content-Type", "Content-Length", "Accept", "X-Demo-Trace",
};
#define NALLOWED (sizeof(allowed) / sizeof(allowed[0]))

static const char menu_json[] =
    "["
    "{\"id\":\"americano\",\"name\":\"아메리카노\",\"price\":4500,"
    "\"desc\":\"고소한 원두를 진하게 내린 기본 커피\",\"image\":\"/img/americano.png\"},"
    "{\"id\":\"latte\",\"name\":\"카페라테\",\"price\":5000,"
    "\"desc\":\"에스프레소에 우유를 부드럽게 더한 커피\",\"image\":\"/img/latte.png\"},"
    "{\"id\":\"lemonade\",\"name\":\"레몬에이드\",\"price\":5500,"
    "\"desc\":\"레몬즙과 탄산수로 만든 상큼한 음료\",\"image\":\"/img/lemonade.png\"}"
    "]";

static void reply(int fd, int status, const char *reason, const char *body)
{
    char head[320];
    size_t len = strlen(body);
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
                     "Content-Length: %zu\r\nCache-Control: no-store\r\nX-Demo-Backend: %s\r\n"
                     "Connection: close\r\n\r\n",
                     status, reason, len, name);
    if (write(fd, head, (size_t)n) == n) {
        (void)!write(fd, body, len);
    }
}

/* JSON 문자열에 넣을 수 있게 출력 가능한 ASCII만 남긴다. */
static void sanitize(char *s)
{
    for (; *s; s++) {
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) {
            *s = '_';
        }
    }
}

/* ---- 받은 요청을 JSON으로 되돌려 주는 부분 ----
 * 값은 JSON 문자열로 escape해 원문 그대로 보인다(따옴표·역슬래시·제어 문자 포함).
 * ASCII 밖 바이트만 U+FFFD로 바꾸고 그 개수를 replaced_bytes로 알린다.
 * 출력 공간이 모자라면 그 항목을 통째로 빼고 omitted에 이름을 적는다.
 * JSON 뼈대(괄호·키·null 대체값·목록)는 값과 별도로 RECV_RESERVE 안에 쓰므로 값이 공간을 다 써도 구조가 완성된다.
 * 뼈대마저 못 쓰면(설계상 일어나지 않아야 함) 잘린 JSON 대신 호출자가 500을 보낸다. */
#define RECV_JSON_SIZE 4096
/* 뼈대 최악 크기: 요청 줄 대체값·headers 여는 괄호 약 53 + 숫자 꼬리 약 70
 * + repeated·too_many 목록(이름 6개) 각 약 90 + omitted 목록(이름 9개) 약 115 + 닫기 = 약 420 < 512 */
#define RECV_RESERVE 512
#define MAX_VALUES 128   /* 같은 이름 머리는 처음 128줄까지만 담고, 넘치면 too_many에 이름을 적는다 */

typedef struct {
    char *buf;
    size_t cap;   /* 값(머리·요청 줄 값)은 이 길이까지만 쓴다 */
    size_t len;
    size_t limit; /* 뼈대까지 포함한 절대 한계(NUL 자리 제외) */
    int bad;      /* 뼈대를 못 쓴 적이 있으면 1: 결과 JSON을 쓰면 안 된다 */
} jw;

static size_t esc_len(const char *s, size_t n)
{
    size_t i, out = 0;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') out += 2;
        else if (c < 0x20 || c == 0x7f || c >= 0x80) out += 6;
        else out += 1;
    }
    return out;
}

static void esc_put(jw *w, const char *s, size_t n, int *replaced)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            w->buf[w->len++] = '\\';
            w->buf[w->len++] = (char)c;
        } else if (c >= 0x80) {
            memcpy(w->buf + w->len, "\\ufffd", 6);
            w->len += 6;
            (*replaced)++;
        } else if (c < 0x20 || c == 0x7f) {
            w->len += (size_t)snprintf(w->buf + w->len, 7, "\\u%04x", c);
        } else {
            w->buf[w->len++] = (char)c;
        }
    }
}

/* 공간이 있을 때만 raw 글자를 쓴다. */
static int jw_raw(jw *w, const char *s)
{
    size_t n = strlen(s);

    if (w->len + n > w->cap) return -1;
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    return 0;
}

/* prefix와 "조각1, 조각2…" 문자열을 한 번에 쓴다. 다 들어가지 않으면 아무것도 쓰지 않고 -1. */
static int jw_value(jw *w, const char *prefix, const char *const *part, const size_t *plen, int count, int *replaced)
{
    size_t need = strlen(prefix) + 2;
    int i;

    for (i = 0; i < count; i++) need += esc_len(part[i], plen[i]) + (i ? 2 : 0);
    if (w->len + need > w->cap) return -1;
    (void)jw_raw(w, prefix);
    w->buf[w->len++] = '"';
    for (i = 0; i < count; i++) {
        if (i) { w->buf[w->len++] = ','; w->buf[w->len++] = ' '; }
        esc_put(w, part[i], plen[i], replaced);
    }
    w->buf[w->len++] = '"';
    return 0;
}

/* JSON 뼈대를 쓴다. 값 한도(cap)가 아니라 절대 한계(limit)까지 쓸 수 있고, 그래도 안 되면 bad를 세운다. */
static void jw_must(jw *w, const char *s)
{
    size_t n = strlen(s);

    if (w->bad || w->len + n > w->limit) {
        w->bad = 1;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void jw_names(jw *w, const char *key, const char *const *names, int count)
{
    int i;

    jw_must(w, key);
    for (i = 0; i < count; i++) {
        jw_must(w, i ? ",\"" : "\"");
        jw_must(w, names[i]);
        jw_must(w, "\"");
    }
    jw_must(w, "]");
}

/* 요청 줄과 화이트리스트 머리를 올바른 JSON 객체로 쓴다. req는 머리 끝(빈 줄 앞)까지의 문자열이다.
 * 같은 이름의 머리가 여러 줄이면 ", "로 이어 한 값으로 보이고 repeated에 이름을 적는다.
 * 성공하면 0, 뼈대를 다 쓰지 못했으면 -1(out을 쓰면 안 된다). */
static int received_json(const char *req, char *out, size_t size)
{
    jw w = {out, size - RECV_RESERVE, 0, size - 1, 0};
    const char *eol = strstr(req, "\r\n"), *p, *sp1, *sp2;
    const char *vals[NALLOWED][MAX_VALUES];
    size_t lens[NALLOWED][MAX_VALUES];
    int nval[NALLOWED] = {0};
    const char *omitted[NALLOWED + 3], *repeated[NALLOWED], *too_many[NALLOWED]; /* omitted: 요청 줄 3칸 + 머리 이름 */
    int over[NALLOWED] = {0};
    int nomit = 0, nrep = 0, nmany = 0, other = 0, replaced = 0, first = 1;
    size_t i;
    char tail[96];

    if (eol == NULL) eol = req + strlen(req);
    /* 요청 줄: 메서드 SP 대상 SP 버전 */
    sp1 = memchr(req, ' ', (size_t)(eol - req));
    sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
    {
        const char *m = req, *t = sp1 ? sp1 + 1 : eol, *v = sp2 ? sp2 + 1 : eol;
        size_t ml = (size_t)((sp1 ? sp1 : eol) - req), tl = (size_t)((sp2 ? sp2 : eol) - t), vl = (size_t)(eol - v);

        jw_must(&w, "{");
        if (jw_value(&w, "\"method\":", &m, &ml, 1, &replaced) != 0) { jw_must(&w, "\"method\":null"); omitted[nomit++] = "method"; }
        if (jw_value(&w, ",\"path\":", &t, &tl, 1, &replaced) != 0) { jw_must(&w, ",\"path\":null"); omitted[nomit++] = "path"; }
        if (jw_value(&w, ",\"version\":", &v, &vl, 1, &replaced) != 0) { jw_must(&w, ",\"version\":null"); omitted[nomit++] = "version"; }
        jw_must(&w, ",\"headers\":{");
    }

    /* 머리 줄을 이름별로 모은다(값 앞뒤 공백 제외). */
    for (p = (*eol ? eol + 2 : eol); *p; ) {
        const char *le = strstr(p, "\r\n"), *colon, *v, *ve;

        if (le == NULL) le = p + strlen(p);
        colon = memchr(p, ':', (size_t)(le - p));
        if (colon != NULL) {
            size_t nl = (size_t)(colon - p);
            for (i = 0; i < NALLOWED; i++) {
                if (strlen(allowed[i]) == nl && strncasecmp(p, allowed[i], nl) == 0) break;
            }
            if (i == NALLOWED) {
                other++;
            } else if (nval[i] < MAX_VALUES) {
                for (v = colon + 1; v < le && (*v == ' ' || *v == '\t'); v++) {
                }
                for (ve = le; ve > v && (ve[-1] == ' ' || ve[-1] == '\t'); ve--) {
                }
                vals[i][nval[i]] = v;
                lens[i][nval[i]] = (size_t)(ve - v);
                nval[i]++;
            } else {
                over[i] = 1; /* 129번째 줄부터는 담지 않는다 */
            }
        }
        p = *le ? le + 2 : le;
    }
    for (i = 0; i < NALLOWED; i++) {
        char prefix[40];

        if (nval[i] == 0) continue;
        snprintf(prefix, sizeof(prefix), "%s\"%s\":", first ? "" : ",", allowed[i]);
        if (jw_value(&w, prefix, vals[i], lens[i], nval[i], &replaced) != 0) {
            omitted[nomit++] = allowed[i];
            continue;
        }
        first = 0;
        if (nval[i] > 1) repeated[nrep++] = allowed[i];
        if (over[i]) too_many[nmany++] = allowed[i];
    }
    /* 여기부터는 남겨 둔 자리에 쓰는 뼈대다 */
    snprintf(tail, sizeof(tail), "},\"other_header_count\":%d,\"replaced_bytes\":%d", other, replaced);
    jw_must(&w, tail);
    jw_names(&w, ",\"repeated\":[", repeated, nrep);
    jw_names(&w, ",\"omitted\":[", omitted, nomit);
    jw_names(&w, ",\"too_many\":[", too_many, nmany);
    jw_must(&w, "}");
    w.buf[w.len] = '\0';
    return w.bad ? -1 : 0;
}

/* 조립한 본문이 버퍼에 다 들어갔을 때만 200을 보낸다. 넘쳤으면 잘린 JSON 대신 500을 보낸다. */
static void reply_ok(int fd, int n, size_t cap, const char *body)
{
    if (n < 0 || (size_t)n >= cap) {
        reply(fd, 500, "Internal Server Error", "{\"error\":\"response too large\"}\n");
        return;
    }
    reply(fd, 200, "OK", body);
}

static void serve(int fd)
{
    char req[8192], method[16], target[1024], recv_json[RECV_JSON_SIZE], body[6144];
    size_t used = 0;
    char *end = NULL;
    struct timeval tv = {5, 0};

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (used < sizeof(req) - 1) {
        ssize_t r = read(fd, req + used, sizeof(req) - 1 - used);
        if (r <= 0) {
            return;
        }
        used += (size_t)r;
        req[used] = '\0';
        if ((end = strstr(req, "\r\n\r\n")) != NULL) {
            break;
        }
    }
    if (end == NULL || sscanf(req, "%15s %1023s", method, target) != 2) {
        reply(fd, 400, "Bad Request", "{\"error\":\"bad request\"}\n");
        return;
    }
    end[2] = '\0'; /* 머리까지만 본다(마지막 머리 줄의 CRLF는 남긴다) */
    sanitize(method);
    sanitize(target);
    printf("백엔드 %s: %s %s\n", name, method, target);
    fflush(stdout);
    if (received_json(req, recv_json, sizeof(recv_json)) != 0) {
        reply(fd, 500, "Internal Server Error", "{\"error\":\"received summary did not fit\"}\n");
        return;
    }

    if (strcmp(target, "/menu") == 0) {
        reply_ok(fd, snprintf(body, sizeof(body), "{\"instance\":\"%s\",\"port\":%d,\"menu\":%s,\"received\":%s}\n",
                              name, port, menu_json, recv_json), sizeof(body), body);
    } else if (strcmp(target, "/inspect") == 0) {
        reply_ok(fd, snprintf(body, sizeof(body), "{\"instance\":\"%s\",\"port\":%d,\"received\":%s}\n", name, port, recv_json),
                 sizeof(body), body);
    } else if (strcmp(target, "/identity.json") == 0) {
        snprintf(body, sizeof(body),
                 "{\"backend\":\"%s\",\"port\":%d,\"pid\":%ld,\"method\":\"%s\",\"received_path\":\"%s\"}\n",
                 name, port, (long)getpid(), method, target);
        reply(fd, 200, "OK", body);
    } else if (strncmp(target, "/slow", 5) == 0 && (target[5] == '\0' || target[5] == '?')) {
        const char *q = strstr(target, "ms=");
        long ms = q != NULL ? strtol(q + 3, NULL, 10) : 3000;
        struct timespec ts;

        if (ms < 0) ms = 0;
        if (ms > 10000) ms = 10000;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        }
        reply_ok(fd, snprintf(body, sizeof(body), "{\"instance\":\"%s\",\"port\":%d,\"waited_ms\":%ld,\"received\":%s}\n",
                              name, port, ms, recv_json), sizeof(body), body);
    } else {
        snprintf(body, sizeof(body), "{\"instance\":\"%s\",\"error\":\"not found\",\"received_path\":\"%s\"}\n",
                 name, target);
        reply(fd, 404, "Not Found", body);
    }
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    int one = 1, lfd;

    if (argc != 3 || (port = atoi(argv[1])) <= 0 || port > 65535) {
        fprintf(stderr, "사용법: %s <포트> <이름>\n", argv[0]);
        return 2;
    }
    sanitize(argv[2]);
    name = argv[2];
    signal(SIGCHLD, SIG_IGN); /* 자식 자동 회수 */
    signal(SIGPIPE, SIG_IGN); /* 프록시가 먼저 끊어도(504) 죽지 않는다 */

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(0x7f000001); /* 같은 호스트·컨테이너의 서버만 접속 */
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(lfd, 64) != 0) {
        perror("backend: listen");
        return 1;
    }
    printf("백엔드 %s: 127.0.0.1:%d 대기\n", name, port);
    fflush(stdout);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        pid_t pid;

        if (fd < 0) {
            if (errno != EINTR) perror("backend: accept");
            continue;
        }
        pid = fork();
        if (pid == 0) {
            close(lfd);
            serve(fd);
            close(fd);
            _exit(0);
        }
        if (pid < 0) {
            perror("backend: fork");
        }
        close(fd);
    }
}
