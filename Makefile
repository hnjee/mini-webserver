# 기본 빌드, 디버그(ASan) 빌드, 단위 테스트, 공통 테스트 파일, SO_REUSEADDR 실험(Step 1).
# echo 모듈은 서버 소스에서 빠졌고 Step 1 회귀 시험(test_echo)에만 쓴다(D-13).
CC      ?= cc
CSTD    ?= -std=c11
WARN    ?= -Wall -Wextra -Wpedantic
CFLAGS  ?= -O2
LDFLAGS ?=

DEBUG_CFLAGS  = -O0 -g -fsanitize=address -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address

TARGET      = webserver
DEBUG_TARGET = webserver-debug
SRC         = src/main.c src/options.c src/listener.c src/server_loop.c src/http_conn.c src/http_parser.c \
              src/http_response.c src/io.c src/static_file.c src/mime.c \
              src/route.c src/proxy.c
HDR         = src/options.h src/listener.h src/server_loop.h src/http_conn.h src/http_parser.h src/http_response.h \
              src/io.h src/static_file.h src/mime.h src/route.h src/proxy.h
TEST_TARGETS = tests/test_options tests/test_echo tests/test_http_parser tests/test_http_response tests/test_http_conn \
               tests/test_mime tests/test_static_file tests/test_server_loop tests/test_route tests/test_proxy tests/test_proxy_conn
EXPERIMENT  = experiments/bind_no_reuse

WWW        = www

.PHONY: all debug test experiment fixtures clean help FORCE

# bind mount로 호스트(macOS) 산출물이 섞일 수 있다. 플랫폼이 바뀌면 이 파일이 갱신되어 전부 다시 빌드된다.
BUILD_STAMP    = .build-platform
BUILD_PLATFORM := $(shell uname -sm)

$(BUILD_STAMP): FORCE
	@echo '$(BUILD_PLATFORM)' | cmp -s - $@ || echo '$(BUILD_PLATFORM)' > $@

all: $(TARGET)

$(TARGET): $(SRC) $(HDR) $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# 디버그 빌드는 AddressSanitizer를 켠다.
debug: $(DEBUG_TARGET)

$(DEBUG_TARGET): $(SRC) $(HDR) $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ $(SRC) $(DEBUG_LDFLAGS)

# 단위 테스트도 ASan으로 빌드해 실행한다.
test: $(TEST_TARGETS)
	./tests/test_options
	./tests/test_echo
	./tests/test_http_parser
	./tests/test_http_response
	./tests/test_http_conn
	./tests/test_mime
	./tests/test_static_file
	./tests/test_server_loop
	./tests/test_route
	./tests/test_proxy
	./tests/test_proxy_conn

tests/test_options: tests/test_options.c src/options.c src/options.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_options.c src/options.c $(DEBUG_LDFLAGS)

tests/test_echo: tests/test_echo.c src/echo.c src/echo.h src/io.c src/io.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_echo.c src/echo.c src/io.c $(DEBUG_LDFLAGS)

tests/test_http_parser: tests/test_http_parser.c src/http_parser.c src/http_parser.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_http_parser.c src/http_parser.c $(DEBUG_LDFLAGS)

tests/test_http_response: tests/test_http_response.c src/http_response.c src/http_response.h src/io.c src/io.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_http_response.c src/http_response.c src/io.c $(DEBUG_LDFLAGS)

HTTP_CONN_SRC = src/http_conn.c src/http_parser.c src/http_response.c src/io.c src/static_file.c src/mime.c \
                src/route.c src/proxy.c
HTTP_CONN_HDR = src/http_conn.h src/http_parser.h src/http_response.h src/io.h src/static_file.h src/mime.h \
                src/route.h src/proxy.h
tests/test_http_conn: tests/test_http_conn.c $(HTTP_CONN_SRC) $(HTTP_CONN_HDR) $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_http_conn.c $(HTTP_CONN_SRC) $(DEBUG_LDFLAGS)

tests/test_mime: tests/test_mime.c src/mime.c src/mime.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_mime.c src/mime.c $(DEBUG_LDFLAGS)

STATIC_FILE_SRC = src/static_file.c src/mime.c src/io.c
tests/test_static_file: tests/test_static_file.c $(STATIC_FILE_SRC) src/static_file.h src/mime.h src/io.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_static_file.c $(STATIC_FILE_SRC) $(DEBUG_LDFLAGS)

# TC-5-11: accept 재시도 오류는 링크 단계 --wrap으로 주입한다(제품 코드에 시험용 분기 없음).
tests/test_server_loop: tests/test_server_loop.c src/server_loop.c src/server_loop.h $(HTTP_CONN_SRC) $(HTTP_CONN_HDR) $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_server_loop.c src/server_loop.c $(HTTP_CONN_SRC) $(DEBUG_LDFLAGS) -Wl,--wrap=accept4

ROUTE_SRC = src/route.c
tests/test_route: tests/test_route.c $(ROUTE_SRC) src/route.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_route.c $(ROUTE_SRC) $(DEBUG_LDFLAGS)

tests/test_proxy: tests/test_proxy.c src/proxy.c src/proxy.h src/http_parser.c src/http_parser.h src/http_response.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_proxy.c src/proxy.c src/http_parser.c $(DEBUG_LDFLAGS)

# Step 6: 백엔드 소켓은 링크 단계 --wrap으로 socketpair를 돌려받아 시험이 연기한다(제품 코드에 시험용 분기 없음).
tests/test_proxy_conn: tests/test_proxy_conn.c $(HTTP_CONN_SRC) $(HTTP_CONN_HDR) $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_proxy_conn.c $(HTTP_CONN_SRC) $(DEBUG_LDFLAGS) \
		-Wl,--wrap=socket -Wl,--wrap=connect -Wl,--wrap=getsockopt

# D-06 실험: SO_REUSEADDR 없이 bind만 해 본다. 서버 소스와 분리한 별도 프로그램이다(빌드만 한다).
experiment: $(EXPERIMENT)

$(EXPERIMENT): experiments/bind_no_reuse.c $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) -o $@ experiments/bind_no_reuse.c $(LDFLAGS)

# 테스트용 www는 컨테이너 내부 Linux 전용 폴더에 만든다. bind mount(Docker Desktop)는 권한 000을 실제로 강제하지 못한다.
# 폴더 위치와 large.bin 크기(4MB)는 recipe 안의 고정값이다(make 변수로 덮어쓸 수 없다).
# 같은 컨테이너에서는 이 폴더를 재사용하고 아래 7개 파일과 sub/index.html만 rm -f 후 새로 쓴다(링크면 링크만 지워진다).
# 하위 폴더 sub/, empty/는 만들기만 하고 empty/index.html은 지운다(D-32). 링크이거나 폴더가 아니면 실패한다.
# 호스트 $(WWW)/는 읽기만 한다. stdout에는 폴더 경로 한 줄만 쓴다: dir=$$(make -s fixtures)
FIXTURE_COPY = index.html style.css app.js image.png photo.jpg sub/index.html

fixtures:
	@set -e; dir=/home/jungle/mini-webserver-www; \
	if [ "$$(id -u)" -eq 0 ]; then echo "root로는 준비하지 않는다(권한 000이 무시된다)" >&2; exit 1; fi; \
	if [ -L "$$dir" ]; then echo "$$dir 가 심볼릭 링크다. 확인 후 직접 지워라" >&2; exit 1; fi; \
	mkdir -p "$$dir"; \
	if [ -L "$$dir" ] || [ ! -d "$$dir" ]; then echo "$$dir 가 실제 디렉터리가 아니다" >&2; exit 1; fi; \
	for d in sub empty; do \
	  if [ -L "$$dir/$$d" ]; then echo "$$dir/$$d 가 심볼릭 링크다. 확인 후 직접 지워라" >&2; exit 1; fi; \
	  mkdir -p "$$dir/$$d"; \
	  if [ -L "$$dir/$$d" ] || [ ! -d "$$dir/$$d" ]; then echo "$$dir/$$d 가 실제 디렉터리가 아니다" >&2; exit 1; fi; \
	done; \
	rm -f -- "$$dir/empty/index.html"; \
	for f in $(FIXTURE_COPY); do rm -f -- "$$dir/$$f"; cp -- "$(WWW)/$$f" "$$dir/$$f"; done; \
	rm -f -- "$$dir/large.bin"; \
	dd if=/dev/urandom of="$$dir/large.bin" bs=1048576 count=4 status=none; \
	rm -f -- "$$dir/secret.txt"; \
	echo "secret: 이 내용이 응답에 보이면 안 된다(403 검증용)." > "$$dir/secret.txt"; \
	chmod 000 "$$dir/secret.txt"; \
	if [ -r "$$dir/secret.txt" ]; then echo "secret.txt가 여전히 읽힌다" >&2; exit 1; fi; \
	echo "준비 완료 (secret.txt 권한 000, large.bin 4MB, sub/index.html, 빈 empty/). 실행: ./webserver -p 8080 -r $$dir" >&2; \
	echo "$$dir"

clean:
	rm -f $(TARGET) $(DEBUG_TARGET) $(TEST_TARGETS) $(EXPERIMENT) $(BUILD_STAMP)
	rm -rf $(TARGET).dSYM $(DEBUG_TARGET).dSYM $(addsuffix .dSYM,$(TEST_TARGETS))

help:
	@echo "make          기본 빌드 ($(TARGET))"
	@echo "make debug    디버그 빌드 ($(DEBUG_TARGET), AddressSanitizer)"
	@echo "make test     단위 테스트 빌드 후 실행 (test_options, test_echo, test_http_parser, test_http_response, test_http_conn, test_mime, test_static_file, test_server_loop, test_route, test_proxy, test_proxy_conn)"
	@echo "make experiment SO_REUSEADDR 없는 bind 실험 프로그램 빌드 ($(EXPERIMENT))"
	@echo "make fixtures 컨테이너 내부 /home/jungle/mini-webserver-www에 테스트용 www 준비, 경로를 stdout에 출력"
	@echo "make clean    빌드 산출물 삭제"
