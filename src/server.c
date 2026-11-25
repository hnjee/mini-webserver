/*
 * 서버: docs/design/step1.md ~ step5.md
 * 프로세스 하나, 스레드 하나가 epoll로 모든 연결을 돌봐요 (Step 5).
 * 연결마다 conn에 상태를 들고 있다가, 커널이 준비됐다고 알려 준 연결만 할 수 있는 만큼 처리하고 돌아와요.
 */
#include "server.h"

#include "file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DRAIN_LIMIT (16 * 1024)
#define FILE_BUF_SIZE 8192

int server_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- 작은 도우미 ---- */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        perror("fcntl O_NONBLOCK");
}

/* 관심 목록에서 이 연결이 지켜볼 일을 바꿔요 (EPOLLIN 또는 EPOLLOUT) */
static void watch(struct server *s, struct conn *c, unsigned events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = c;
    if (epoll_ctl(s->ep, EPOLL_CTL_MOD, c->fd, &ev) < 0)
        perror("epoll_ctl MOD");
}

/* path가 name과 같은지. ? 뒤 쿼리 문자열은 무시해요 */
static int path_is(const char *path, const char *name)
{
    size_t n = strlen(name);
    return strncmp(path, name, n) == 0 && (path[n] == '\0' || path[n] == '?');
}

/* 이번 요청의 응답 자원(out, 파일, 본문)을 정리해요 */
static void reset_response(struct conn *c)
{
    free(c->out);
    c->out = NULL;
    c->out_len = c->out_sent = 0;
    if (c->file_fd >= 0)
        close(c->file_fd);
    c->file_fd = -1;
    c->file_off = c->file_size = 0;
    free(c->body);
    c->body = NULL;
    c->body_len = 0;
}

/* ---- 응답 준비: 바로 보내지 않고 out에 채워요 ---- */

/* 오류 응답. 본문은 "코드 문구\n", extra는 "Allow: GET" 같은 헤더 한 줄이거나 NULL */
static int prepare_error(struct conn *c, int status, const char *extra)
{
    char body[64];
    int body_len = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
    c->out = malloc(512);
    if (c->out == NULL)
        return -1;
    int n = snprintf(c->out, 512,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %d\r\n"
                     "%s%s"
                     "Connection: %s\r\n"
                     "\r\n"
                     "%s",
                     status, http_reason(status), body_len,
                     extra ? extra : "", extra ? "\r\n" : "",
                     c->keep ? "keep-alive" : "close", body);
    c->out_len = (size_t)n;
    return 0;
}

/* 에코: 헤더와 받은 본문을 out에 한 번에 담아요 (S4-07) */
static int prepare_echo(struct conn *c)
{
    const char *type = http_find_header(&c->req, "Content-Type");
    char head[HEADER_BUF_SIZE];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     type != NULL ? type : "application/octet-stream",
                     c->body_len, c->keep ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= sizeof(head))
        return prepare_error(c, 500, NULL);
    c->out = malloc((size_t)n + c->body_len);
    if (c->out == NULL)
        return -1;
    memcpy(c->out, head, (size_t)n);
    if (c->body_len > 0)
        memcpy(c->out + n, c->body, c->body_len);
    c->out_len = (size_t)n + c->body_len;
    return 0;
}

/* 정적 파일: 파일을 열어 들고 200 헤더만 out에 써요. 본문은 flush()가 조금씩 보내요.
   0: 준비됨, 403·404·500: 오류 응답을 보낼 것 */
static int prepare_file(struct conn *c, const char *file)
{
    int ffd = open(file, O_RDONLY);
    if (ffd < 0)
        return errno == ENOENT ? 404 : errno == EACCES ? 403 : 500;
    struct stat st;
    if (fstat(ffd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(ffd);
        return 403;
    }
    c->out = malloc(256);
    if (c->out == NULL) {
        close(ffd);
        return 500;
    }
    int n = snprintf(c->out, 256,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     mime_type(file), (long long)st.st_size, c->keep ? "keep-alive" : "close");
    c->out_len = (size_t)n;
    c->file_fd = ffd;
    c->file_off = 0;
    c->file_size = st.st_size;
    return 0;
}

/* 경로와 메서드에 맞는 응답을 준비해요. 성공 0, 메모리 부족 -1 */
static int prepare_response(struct server *s, struct conn *c)
{
    int post = strcmp(c->req.method, "POST") == 0;
    int r;
    if (path_is(c->req.path, "/echo"))
        r = post ? prepare_echo(c) : prepare_error(c, 405, "Allow: POST");
    else if (post)
        r = prepare_error(c, 405, "Allow: GET");
    else {
        char file[PATH_MAX];
        int status = resolve_path(s->root, c->req.path, file, sizeof(file));
        if (status == 0)
            status = prepare_file(c, file);
        r = status == 0 ? 0 : prepare_error(c, status, NULL);
    }
    free(c->body);                               /* 에코는 out에 복사해 뒀어요 */
    c->body = NULL;
    c->body_len = 0;
    return r;
}

/* 본문 길이를 정해요. 0: c->cl에 길이, 그 밖: 경계를 알 수 없어 보낼 상태 코드 (400·411·413·501) */
static int body_length(struct conn *c)
{
    if (http_find_header(&c->req, "Transfer-Encoding") != NULL)
        return 501;
    int r = http_content_length(&c->req, &c->cl);
    if (r < 0)
        return 400;
    if (r == 0 && strcmp(c->req.method, "POST") == 0)
        return 411;
    if (c->cl > HTTP_MAX_BODY)
        return 413;
    return 0;
}

/* ---- 보내기 ---- */

enum { FLUSH_DONE, FLUSH_WAIT, FLUSH_CLOSED };

/* out, 그다음 파일을 보낼 수 있는 만큼 보내요.
   FLUSH_DONE: 다 보냄(keep이면 다음 요청 준비됨), FLUSH_WAIT: 보내기 버퍼가 참, FLUSH_CLOSED: 연결을 닫음(c 해제됨) */
static int flush(struct server *s, struct conn *c)
{
    char fbuf[FILE_BUF_SIZE];
    for (;;) {
        const char *p;
        size_t n;
        if (c->out_sent < c->out_len) {
            p = c->out + c->out_sent;
            n = c->out_len - c->out_sent;
        } else if (c->file_fd >= 0 && c->file_off < c->file_size) {
            ssize_t r = pread(c->file_fd, fbuf, sizeof(fbuf), c->file_off);
            if (r <= 0) {
                if (r < 0 && errno == EINTR)
                    continue;
                perror("pread");
                server_close_conn(s, c);     /* 헤더는 이미 나감: 닫기만 */
                return FLUSH_CLOSED;
            }
            p = fbuf;
            n = (size_t)r;
        } else {
            break;                           /* 다 보냄 */
        }

        ssize_t w = write(c->fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!c->want_out) {
                    watch(s, c, EPOLLOUT);   /* 자리가 나면 알려 줘 */
                    c->want_out = 1;
                }
                return FLUSH_WAIT;
            }
            server_close_conn(s, c);         /* EPIPE, ECONNRESET: 클라이언트가 떠남 */
            return FLUSH_CLOSED;
        }
        c->last_active = time(NULL);
        if (c->out_sent < c->out_len)
            c->out_sent += (size_t)w;
        else
            c->file_off += w;                /* 실제로 쓴 만큼만 (S5-05) */
    }

    if (c->want_out) {
        watch(s, c, EPOLLIN);
        c->want_out = 0;
    }
    reset_response(c);
    if (!c->keep) {
        server_close_conn(s, c);
        return FLUSH_CLOSED;
    }
    /* 이 요청이 쓴 바이트를 빼고 남은 바이트(다음 요청)를 앞으로 당겨요 */
    memmove(c->buf, c->buf + c->hdr_end, c->len - c->hdr_end);
    c->len -= c->hdr_end;
    c->hdr_end = 0;
    c->state = ST_HEADER;
    return FLUSH_DONE;
}

/* 응답을 준비해 보내기 시작해요 */
static int start_write(struct server *s, struct conn *c)
{
    c->state = ST_WRITE;
    return flush(s, c);
}

/* ---- 읽기 ---- */

/* 버퍼에 있는 것으로 할 수 있는 만큼 하고, 모자라면 더 읽기를 EAGAIN까지 반복해요 */
static void advance(struct server *s, struct conn *c)
{
    for (;;) {
        char *dst;
        size_t room;

        if (c->state == ST_HEADER) {
            size_t end = http_find_header_end(c->buf, c->len);
            if (end == 0 && c->len == sizeof(c->buf))
                end = (size_t)-1;            /* 버퍼가 꽉 찼는데 헤더의 끝이 없음 */
            if (end != 0) {
                int status = end == (size_t)-1 ? 431 : http_parse_request(c->buf, end, &c->req);
                if (status == 0)
                    status = body_length(c);
                if (status != 0) {           /* 경계를 모름: 오류 응답 뒤 닫기 */
                    c->keep = 0;
                    c->hdr_end = c->len;
                    if (prepare_error(c, status, NULL) < 0) {
                        server_close_conn(s, c);
                        return;
                    }
                    start_write(s, c);
                    return;
                }
                c->keep = http_keep_alive(&c->req);
                size_t have = c->len - end;
                if (have > c->cl)
                    have = c->cl;            /* 그 뒤는 다음 요청 */
                c->hdr_end = end + have;     /* 이 요청이 헤더 버퍼에서 쓴 바이트 수 */
                if (c->cl > 0) {
                    c->body = malloc(c->cl);
                    if (c->body == NULL) {
                        perror("malloc");
                        server_close_conn(s, c);
                        return;
                    }
                    memcpy(c->body, c->buf + end, have);
                    c->body_len = have;
                }
                c->state = ST_BODY;
                continue;
            }
            dst = c->buf + c->len;
            room = sizeof(c->buf) - c->len;
        } else if (c->state == ST_BODY) {
            if (c->body_len == c->cl) {
                if (prepare_response(s, c) < 0) {
                    server_close_conn(s, c);
                    return;
                }
                if (start_write(s, c) != FLUSH_DONE)
                    return;
                continue;                    /* 다음 요청이 이미 버퍼에 있을 수 있음 (파이프라이닝) */
            }
            dst = c->body + c->body_len;
            room = c->cl - c->body_len;
        } else {
            return;                          /* 보내는 중에는 읽지 않아요 */
        }

        ssize_t n = read(c->fd, dst, room);
        if (n > 0) {
            c->last_active = time(NULL);
            if (c->state == ST_HEADER)
                c->len += (size_t)n;
            else
                c->body_len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;                          /* 지금은 없음: 다음 알림까지 손 뗌 */
        if (n < 0)
            perror("read");
        server_close_conn(s, c);             /* 0: 클라이언트가 끊음, 그 밖: 실패 */
        return;
    }
}

/* ---- 밖에 여는 함수 ---- */

int server_init(struct server *s, int listen_fd, const char *root)
{
    memset(s, 0, sizeof(*s));
    s->listen_fd = listen_fd;
    s->root = root;
    s->ep = epoll_create1(0);
    if (s->ep < 0) {
        perror("epoll_create1");
        return -1;
    }
    set_nonblock(listen_fd);
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;                      /* NULL이면 리슨 소켓 */
    if (epoll_ctl(s->ep, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl ADD listen");
        close(s->ep);
        s->ep = -1;
        return -1;
    }
    return 0;
}

int server_on_accept(struct server *s)
{
    int count = 0;
    for (int i = 0; i < ACCEPT_BATCH; i++) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;                       /* 줄이 빔 */
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            break;
        }
        set_nonblock(fd);

        struct conn *c = calloc(1, sizeof(*c));
        if (c == NULL) {
            perror("calloc");
            close(fd);
            continue;
        }
        c->fd = fd;
        c->file_fd = -1;
        c->state = ST_HEADER;
        c->last_active = time(NULL);

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = c;                     /* 알림이 오면 이 conn을 돌려받아요 */
        if (epoll_ctl(s->ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("epoll_ctl ADD");
            close(fd);
            free(c);
            continue;
        }
        c->next = s->conns;
        if (s->conns != NULL)
            s->conns->prev = c;
        s->conns = c;
        s->nconn++;
        count++;
    }
    return count;
}

void conn_on_readable(struct server *s, struct conn *c)
{
    advance(s, c);
}

void conn_on_writable(struct server *s, struct conn *c)
{
    if (c->state != ST_WRITE)
        return;
    if (flush(s, c) == FLUSH_DONE && c->len > 0)
        advance(s, c);                       /* 다음 요청이 이미 버퍼에 있음 */
}

void server_close_conn(struct server *s, struct conn *c)
{
    if (epoll_ctl(s->ep, EPOLL_CTL_DEL, c->fd, NULL) < 0)
        perror("epoll_ctl DEL");
    /* 쓰기를 먼저 닫고 남은 데이터를 한 번 읽어 버려요. close()에서 RST가 나가지 않게. 논블로킹이라 기다리지 않아요 */
    shutdown(c->fd, SHUT_WR);
    char tmp[DRAIN_LIMIT];
    while (read(c->fd, tmp, sizeof(tmp)) < 0 && errno == EINTR)
        ;
    if (close(c->fd) < 0)
        perror("close");
    reset_response(c);

    if (c->prev != NULL)
        c->prev->next = c->next;
    else
        s->conns = c->next;
    if (c->next != NULL)
        c->next->prev = c->prev;
    s->nconn--;
    free(c);
}

int server_sweep_idle(struct server *s, time_t now)
{
    int closed = 0;
    struct conn *c = s->conns;
    while (c != NULL) {
        struct conn *next = c->next;
        if (now - c->last_active >= IDLE_TIMEOUT_SEC) {
            server_close_conn(s, c);
            closed++;
        }
        c = next;
    }
    return closed;
}

void server_free(struct server *s)
{
    while (s->conns != NULL)
        server_close_conn(s, s->conns);
    if (s->ep >= 0)
        close(s->ep);
    s->ep = -1;
}

void server_run(int listen_fd, const char *root_real)
{
    struct server s;
    if (server_init(&s, listen_fd, root_real) < 0)
        return;

    struct epoll_event evs[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(s.ep, evs, MAX_EVENTS, TICK_MS);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            struct conn *c = evs[i].data.ptr;
            if (c == NULL)
                server_on_accept(&s);                       /* 손님 받기 */
            else if (evs[i].events & (EPOLLHUP | EPOLLERR))
                server_close_conn(&s, c);
            else if (c->state == ST_WRITE)
                conn_on_writable(&s, c);                    /* 이어서 보내기 */
            else
                conn_on_readable(&s, c);                    /* 이어서 읽기 */
        }
        server_sweep_idle(&s, time(NULL));                  /* 15초 넘은 연결 닫기 */
    }
    server_free(&s);
}
