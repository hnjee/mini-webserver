/* Step 3 단위 테스트: static_file (TC-3-06 D-28 표, TC-3-07 접두 경계, TC-3-08 fd 누수, TC-3-19 접두 비교). */
#define _XOPEN_SOURCE 700 /* realpath·PATH_MAX (설계 재검토 I1) */

#include "../src/static_file.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures, skipped;
static char tmp[PATH_MAX];  /* mkdtemp 결과(정규화) */
static char root[PATH_MAX]; /* tmp/www */

static void check(const char *name, int condition, const char *detail)
{
    if (condition) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (%s)\n", name, detail);
        failures++;
    }
}

static void setup_or_die(int ok, const char *what)
{
    if (!ok) {
        printf("  SETUP ERROR %s: %s\n", what, strerror(errno));
        exit(2);
    }
}

static const char *result_name(static_file_result r)
{
    switch (r) {
    case STATIC_FILE_OK: return "OK";
    case STATIC_FILE_REDIRECT: return "REDIRECT";
    case STATIC_FILE_BAD_TARGET: return "BAD_TARGET";
    case STATIC_FILE_FORBIDDEN: return "FORBIDDEN";
    case STATIC_FILE_NOT_FOUND: return "NOT_FOUND";
    case STATIC_FILE_ERROR: return "ERROR";
    }
    return "?";
}

/* 가장 낮은 빈 fd 번호. 호출 전후가 같으면 새로 열린 fd가 남지 않았다(TC-3-08). */
static int lowest_free_fd(void)
{
    int fd = open("/dev/null", O_RDONLY);
    setup_or_die(fd >= 0, "open /dev/null");
    close(fd);
    return fd;
}

static void path_in(char *out, const char *base, const char *rel)
{
    int n = snprintf(out, PATH_MAX, "%s/%s", base, rel);
    setup_or_die(n > 0 && n < PATH_MAX, "경로 조립");
}

static void make_file(const char *rel, const char *data, size_t len, mode_t mode)
{
    char p[PATH_MAX];
    int fd;

    path_in(p, tmp, rel);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    setup_or_die(fd >= 0, p);
    setup_or_die(write(fd, data, len) == (ssize_t)len, "write");
    close(fd);
    setup_or_die(chmod(p, mode) == 0, "chmod");
}

static void make_dir(const char *rel)
{
    char p[PATH_MAX];
    path_in(p, tmp, rel);
    setup_or_die(mkdir(p, 0755) == 0, p);
}

static void make_link(const char *target, const char *rel)
{
    char p[PATH_MAX];
    path_in(p, tmp, rel);
    setup_or_die(symlink(target, p) == 0, p);
}

static const char index_body[] = "<h1>root</h1>\n";
static const char sub_body[] = "<h1>sub</h1>\n";
static const char data_body[] = "\x89PNG\0binary";

static void build_tree(void)
{
    char p[PATH_MAX];

    setup_or_die(mkdtemp(strcpy(p, "/tmp/test_static_file-XXXXXX")) != NULL, "mkdtemp");
    setup_or_die(realpath(p, tmp) != NULL, "realpath tmp");
    path_in(root, tmp, "www");

    make_dir("www");
    make_file("www/index.html", index_body, sizeof(index_body) - 1, 0644);
    make_file("www/a.txt", "hello\n", 6, 0644);
    make_file("www/empty.txt", "", 0, 0644);
    make_file("www/data", data_body, sizeof(data_body) - 1, 0644);
    make_link("data", "www/page.html");
    make_file("www/..a", "x", 1, 0644);
    make_file("www/a..", "x", 1, 0644);
    make_file("www/...", "x", 1, 0644);
    make_dir("www/sub");
    make_file("www/sub/index.html", sub_body, sizeof(sub_body) - 1, 0644);
    make_dir("www/emptydir");
    make_dir("www/idxdir");
    make_dir("www/idxdir/index.html");
    make_dir("www/noperm");
    make_file("www/noperm/x.txt", "x", 1, 0644);
    setup_or_die(chmod(strcat(strcpy(p, root), "/noperm"), 0) == 0, "chmod noperm");
    make_file("www/secret.txt", "secret\n", 7, 0);
    make_link("loop", "www/loop");
    path_in(p, root, "fifo");
    setup_or_die(mkfifo(p, 0644) == 0, "mkfifo");
    /* TC-3-07: 루트 옆 폴더와 그쪽을 가리키는 링크 */
    make_dir("www-evil");
    make_file("www-evil/x.txt", "evil\n", 5, 0644);
    make_link("../www-evil", "www/evil");
    make_link("sub", "www/inner");
    make_dir("www/outidx");
    make_link("../../www-evil/x.txt", "www/outidx/index.html");
}

static void remove_tree(void)
{
    char p[PATH_MAX], cmd[PATH_MAX + 16];

    path_in(p, root, "noperm");
    chmod(p, 0755);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp); /* tmp는 mkdtemp가 만든 따옴표 없는 경로다 */
    if (system(cmd) != 0) {
        printf("  (임시 폴더 정리 실패: %s)\n", tmp);
    }
}

/*
 * static_file_open을 부르고 결과를 확인한다. OK면 size·content_type을 확인한 뒤 fd를 닫는다.
 * 끝나면 TC-3-08 fd 누수(가장 낮은 빈 fd가 호출 전과 같음)를 함께 본다.
 */
static void expect_open(const char *name, const char *rt, const char *target, static_file_result want,
                        long want_size, const char *want_type, const char *want_location)
{
    static static_file f;
    static_file_result r;
    char label[256], detail[PATH_MAX + 128];
    int before = lowest_free_fd();

    memset(&f, 0, sizeof(f));
    f.fd = -1;
    r = static_file_open(rt, target, &f);
    snprintf(label, sizeof(label), "%s → %s", name, result_name(want));
    snprintf(detail, sizeof(detail), "결과 %s", result_name(r));
    check(label, r == want, detail);
    if (r == want && want == STATIC_FILE_OK) {
        snprintf(label, sizeof(label), "%s size %ld", name, want_size);
        check(label, (long)f.size == want_size, "size가 다르다");
        if (want_type != NULL) {
            snprintf(label, sizeof(label), "%s content_type %s", name, want_type);
            check(label, f.content_type != NULL && strcmp(f.content_type, want_type) == 0,
                  f.content_type ? f.content_type : "(NULL)");
        }
    }
    if (r == want && want == STATIC_FILE_REDIRECT) {
        snprintf(label, sizeof(label), "%s location %s", name, want_location);
        check(label, strcmp(f.location, want_location) == 0, f.location);
    }
    if (r == STATIC_FILE_OK) {
        close(f.fd);
    }
    snprintf(label, sizeof(label), "TC-3-08 %s 뒤 fd 누수 없음", name);
    check(label, lowest_free_fd() == before, "열린 fd가 남았다");
}

/* 길이가 정확히 total(root 포함, NUL 제외)이 되는 "/././…/a.txt" 요청 대상을 만든다. */
static void long_target(char *out, size_t cap, size_t total)
{
    size_t rl = strlen(root), want = total - rl, i = 0;
    const char *tail = "/a.txt";

    setup_or_die(want + 1 <= cap && want > strlen(tail) + 1, "긴 경로 크기");
    if ((want - strlen(tail)) % 2 == 1) {
        out[i++] = '/'; /* 홀수 길이는 '/' 하나로 맞춘다(연속 '/'는 같은 경로) */
    }
    while (i + strlen(tail) < want) {
        out[i++] = '/';
        out[i++] = '.';
    }
    strcpy(out + i, tail);
    setup_or_die(rl + strlen(out) == total, "긴 경로 길이 계산");
}

static void test_open_rules(void)
{
    static const char *const html = "text/html; charset=utf-8";
    static char big[PATH_MAX * 2];
    static char longname[300];
    char p[PATH_MAX + 8];
    int root_user = geteuid() == 0;

    /* 1행: '/' 시작 */
    expect_open("TC-3-06 1행 index.html", root, "index.html", STATIC_FILE_BAD_TARGET, 0, NULL, NULL);
    expect_open("TC-3-06 1행 *", root, "*", STATIC_FILE_BAD_TARGET, 0, NULL, NULL);
    expect_open("TC-3-06 1행 절대 형식", root, "http://localhost/", STATIC_FILE_BAD_TARGET, 0, NULL, NULL);
    /* 2행: '?' 제거 */
    expect_open("TC-3-06 2행 /a.txt?v=1", root, "/a.txt?v=1", STATIC_FILE_OK, 6, "text/plain; charset=utf-8",
                NULL);
    expect_open("TC-3-06 2행 /?a=..", root, "/?a=..", STATIC_FILE_OK, (long)sizeof(index_body) - 1, html, NULL);
    expect_open("TC-3-06 2행 /a.txt?x.html MIME은 ? 앞", root, "/a.txt?x.html", STATIC_FILE_OK, 6,
                "text/plain; charset=utf-8", NULL);
    /* 3행: '..' 조각 */
    expect_open("TC-3-06 3행 /..", root, "/..", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    expect_open("TC-3-06 3행 /../../etc/passwd", root, "/../../etc/passwd", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    expect_open("TC-3-06 3행 /sub/../a.txt", root, "/sub/../a.txt", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    expect_open("TC-3-06 3행 /sub/..", root, "/sub/..", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    expect_open("TC-3-06 3행 /../", root, "/../", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    expect_open("TC-3-06 3행 /..a 통과", root, "/..a", STATIC_FILE_OK, 1, NULL, NULL);
    expect_open("TC-3-06 3행 /a.. 통과", root, "/a..", STATIC_FILE_OK, 1, NULL, NULL);
    expect_open("TC-3-06 3행 /... 통과", root, "/...", STATIC_FILE_OK, 1, NULL, NULL);
    expect_open("TC-3-06 3행 /%2e%2e/ 통과(없음)", root, "/%2e%2e/", STATIC_FILE_NOT_FOUND, 0, NULL, NULL);
    expect_open("TC-3-06 3행 ?.. 뒤는 검사하지 않음", root, "/a.txt?/../x", STATIC_FILE_OK, 6, NULL, NULL);
    /* 4행: 루트 + 경로 길이 (PATH_MAX는 NUL 포함 상한) */
    long_target(big, sizeof(big), PATH_MAX - 1);
    expect_open("TC-3-06 4행 루트+경로 PATH_MAX-1바이트 통과", root, big, STATIC_FILE_OK, 6, NULL, NULL);
    long_target(big, sizeof(big), PATH_MAX);
    expect_open("TC-3-06 4행 루트+경로 PATH_MAX바이트 400", root, big, STATIC_FILE_BAD_TARGET, 0, NULL, NULL);
    long_target(big, sizeof(big), PATH_MAX + 1000);
    expect_open("TC-3-06 4행 PATH_MAX 초과 /./././… 400", root, big, STATIC_FILE_BAD_TARGET, 0, NULL, NULL);
    /* 5행: realpath 실패 */
    expect_open("TC-3-06 5행 /nope.html", root, "/nope.html", STATIC_FILE_NOT_FOUND, 0, NULL, NULL);
    expect_open("TC-3-06 5행 /index.html/x (ENOTDIR)", root, "/index.html/x", STATIC_FILE_NOT_FOUND, 0, NULL,
                NULL);
    longname[0] = '/';
    memset(longname + 1, 'a', 256);
    longname[257] = '\0';
    expect_open("TC-3-06 5행 256바이트 이름 400 (ENAMETOOLONG)", root, longname, STATIC_FILE_BAD_TARGET, 0, NULL,
                NULL);
    expect_open("TC-3-06 5행 loop -> loop 500 (ELOOP)", root, "/loop", STATIC_FILE_ERROR, 0, NULL, NULL);
    if (root_user) {
        printf("  건너뜀 TC-3-06 5행 권한 000 디렉터리 아래 403 (root로 실행)\n");
        skipped++;
    } else {
        expect_open("TC-3-06 5행 권한 000 디렉터리 아래 403 (EACCES)", root, "/noperm/x.txt",
                    STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    }
    /* 6행·TC-3-07: 루트 밖 */
    expect_open("TC-3-07 /evil/x.txt (www-evil) 403", root, "/evil/x.txt", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    expect_open("TC-3-07 루트 안 링크 /inner/index.html 200", root, "/inner/index.html", STATIC_FILE_OK,
                (long)sizeof(sub_body) - 1, html, NULL);
    expect_open("TC-3-07 밖을 가리키는 index.html 403", root, "/outidx/", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    /* 7행: 디렉터리 */
    expect_open("TC-3-06 7행 / → index.html", root, "/", STATIC_FILE_OK, (long)sizeof(index_body) - 1, html, NULL);
    expect_open("TC-3-06 7행 /sub 301", root, "/sub", STATIC_FILE_REDIRECT, 0, NULL, "/sub/");
    expect_open("TC-3-06 7행 //sub 301 (C1)", root, "//sub", STATIC_FILE_REDIRECT, 0, NULL, "/sub/");
    expect_open("TC-3-06 7행 /sub?v=1 301 쿼리 없음 (D-24)", root, "/sub?v=1", STATIC_FILE_REDIRECT, 0, NULL,
                "/sub/");
    expect_open("TC-3-06 7행 //inner (디렉터리 링크) 301", root, "//inner", STATIC_FILE_REDIRECT, 0, NULL,
                "/inner/");
    expect_open("TC-3-06 7행 /sub/ 200", root, "/sub/", STATIC_FILE_OK, (long)sizeof(sub_body) - 1, html, NULL);
    expect_open("TC-3-06 7행 /sub/?v=1 200", root, "/sub/?v=1", STATIC_FILE_OK, (long)sizeof(sub_body) - 1, html,
                NULL);
    expect_open("TC-3-06 7행 index 없는 /emptydir/ 403", root, "/emptydir/", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    expect_open("TC-3-06 7행 index.html이 디렉터리 403", root, "/idxdir/", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    /* 8행: 일반 파일이 아닌 대상 (멈추면 alarm으로 실패) */
    expect_open("TC-3-06 8행 FIFO 403, 멈추지 않음 (D-31)", root, "/fifo", STATIC_FILE_FORBIDDEN, 0, NULL, NULL);
    /* 9행: open */
    if (root_user) {
        printf("  건너뜀 TC-3-06 9행 권한 000 파일 403 (root로 실행)\n");
        skipped++;
    } else {
        expect_open("TC-3-06 9행 권한 000 파일 403 (EACCES)", root, "/secret.txt", STATIC_FILE_FORBIDDEN, 0, NULL,
                    NULL);
    }
    /* 10행·MIME(D-34) */
    expect_open("TC-3-06 10행 빈 파일 크기 0", root, "/empty.txt", STATIC_FILE_OK, 0, NULL, NULL);
    expect_open("TC-3-06 D-34 page.html -> data는 text/html", root, "/page.html", STATIC_FILE_OK,
                (long)sizeof(data_body) - 1, html, NULL);
    expect_open("TC-3-06 D-34 /data는 octet-stream", root, "/data", STATIC_FILE_OK, (long)sizeof(data_body) - 1,
                "application/octet-stream", NULL);
    /* 루트가 "/"인 경우(D-26 예외): 절대 경로로 같은 파일을 연다 */
    snprintf(p, sizeof(p), "%s/a.txt", root);
    expect_open("TC-3-06 루트 / 에서 절대 경로", "/", p, STATIC_FILE_OK, 6, NULL, NULL);
}

static void test_within_root(void)
{
    static const struct {
        const char *root, *resolved;
        int want;
    } cases[] = {
        {"/", "/x", 1},
        {"/", "/", 1},
        {"/t/www", "/t/www", 1},
        {"/t/www", "/t/www/a", 1},
        {"/t/www", "/t/www-evil", 0},
        {"/t/www", "/t/www-evil/a", 0},
        {"/t/www", "/t", 0},
        {"/t/www", "/t/ww", 0},
    };
    size_t i;
    char name[128];

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(name, sizeof(name), "TC-3-19 (%s, %s) → %d", cases[i].root, cases[i].resolved, cases[i].want);
        check(name, static_file_within_root(cases[i].root, cases[i].resolved) == cases[i].want, "다르다");
    }
}

/* static_file_send_body: 64KB를 여러 번 넘는 파일의 바이트 일치, 파일이 size보다 짧으면 EIO, 상대가 닫으면 EPIPE. */
static void test_send_body(void)
{
    const size_t len = 3 * STATIC_FILE_CHUNK + 123;
    char *data = malloc(len), *got = malloc(len + 1);
    char p[PATH_MAX];
    static static_file f;
    size_t i, total = 0;
    int fds[2], status, rc, err;
    pid_t pid;

    setup_or_die(data != NULL && got != NULL, "malloc");
    for (i = 0; i < len; i++) {
        data[i] = (char)(i * 131 + 7);
    }
    make_file("www/big.bin", data, len, 0644);
    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    pid = fork();
    setup_or_die(pid >= 0, "fork");
    if (pid == 0) {
        static_file cf;
        close(fds[0]);
        _exit(static_file_open(root, "/big.bin", &cf) == STATIC_FILE_OK && static_file_send_body(fds[1], &cf) == 0
                  ? 0
                  : 1);
    }
    close(fds[1]);
    for (;;) {
        ssize_t n = read(fds[0], got + total, len + 1 - total);
        setup_or_die(n >= 0, "read");
        if (n == 0) {
            break;
        }
        total += (size_t)n;
        if (total == len + 1) {
            break;
        }
    }
    close(fds[0]);
    setup_or_die(waitpid(pid, &status, 0) == pid, "waitpid");
    check("send_body 3×64KB+123 성공(0)", WIFEXITED(status) && WEXITSTATUS(status) == 0, "자식 실패");
    check("send_body 바이트 일치", total == len && memcmp(got, data, len) == 0, "바이트가 다르다");

    /* 파일이 size보다 짧다(전송 중 줄어든 경우와 같은 조건) */
    path_in(p, root, "a.txt");
    f.fd = open(p, O_RDONLY);
    setup_or_die(f.fd >= 0, "open a.txt");
    f.size = 6 + 10;
    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    errno = 0;
    rc = static_file_send_body(fds[1], &f);
    err = errno;
    check("send_body 파일이 size보다 짧으면 -1, errno EIO", rc == -1 && err == EIO, strerror(err));
    total = (size_t)read(fds[0], got, len);
    check("send_body 짧은 파일도 있는 만큼(6바이트)은 보냄", total == 6, "바이트 수가 다르다");
    check("send_body 파일 fd를 닫지 않음", fcntl(f.fd, F_GETFD) != -1, "닫혔다");
    close(f.fd);
    close(fds[0]);

    /* 상대가 닫음 */
    f.fd = open(p, O_RDONLY);
    setup_or_die(f.fd >= 0, "open a.txt");
    f.size = 6;
    close(fds[1]);
    setup_or_die(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    close(fds[0]);
    errno = 0;
    rc = static_file_send_body(fds[1], &f);
    err = errno;
    check("send_body 상대가 닫으면 -1, errno EPIPE", rc == -1 && err == EPIPE, strerror(err));
    close(fds[1]);
    close(f.fd);
    free(data);
    free(got);
}

/* TC-5-10 보조(D-58): 파일 조각 하나 읽기. want 이하만 읽고, 남은 길이가 있는데 파일 끝이면 -1(EIO). */
static void test_read_chunk(void)
{
    char p[PATH_MAX], buf[64], want6[6];
    ssize_t a, b, c;
    int fd, err, fdp;

    path_in(p, root, "a.txt");
    fdp = open(p, O_RDONLY);
    setup_or_die(fdp >= 0 && read(fdp, want6, sizeof(want6)) == (ssize_t)sizeof(want6), "a.txt 내용");
    close(fdp);
    fd = open(p, O_RDONLY);
    setup_or_die(fd >= 0, "open a.txt");
    a = static_file_read_chunk(fd, buf, 4);
    b = static_file_read_chunk(fd, buf + 4, sizeof(buf) - 4);
    errno = 0;
    c = static_file_read_chunk(fd, buf + 6, 10);
    err = errno;
    check("read_chunk want 4 → 4바이트", a == 4, "길이가 다르다");
    check("read_chunk 남은 2바이트만 → 2, 내용 일치", b == 2 && memcmp(buf, want6, 6) == 0, "다르다");
    check("read_chunk 파일 끝(남은 길이 있음) → -1, errno EIO", c == -1 && err == EIO, strerror(err));
    check("read_chunk 파일 fd를 닫지 않음", fcntl(fd, F_GETFD) != -1, "닫혔다");
    close(fd);
}

int main(void)
{
    alarm(10); /* FIFO 등에서 멈추면 끝없이 기다리지 않게 한다 */
    signal(SIGPIPE, SIG_IGN);
    printf("test_static_file (TC-3-06, TC-3-07, TC-3-08, TC-3-19)\n");
    build_tree();
    test_open_rules();
    test_within_root();
    test_send_body();
    test_read_chunk();
    remove_tree();

    if (skipped > 0) {
        printf("건너뜀 %d건 (통과로 세지 않음)\n", skipped);
    }
    if (failures > 0) {
        printf("실패 %d건\n", failures);
        return 1;
    }
    printf("모두 통과\n");
    return 0;
}
