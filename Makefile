# 기본 빌드, 디버그(ASan) 빌드, 단위 테스트, 공통 테스트 파일, SO_REUSEADDR 실험(Step 1).
CC      ?= cc
CSTD    ?= -std=c11
WARN    ?= -Wall -Wextra -Wpedantic
CFLAGS  ?= -O2
LDFLAGS ?=

DEBUG_CFLAGS  = -O0 -g -fsanitize=address -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address

TARGET      = webserver
DEBUG_TARGET = webserver-debug
SRC         = src/main.c src/options.c src/listener.c src/echo.c
HDR         = src/options.h src/listener.h src/echo.h
TEST_TARGETS = tests/test_options tests/test_echo
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

tests/test_options: tests/test_options.c src/options.c src/options.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_options.c src/options.c $(DEBUG_LDFLAGS)

tests/test_echo: tests/test_echo.c src/echo.c src/echo.h $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(DEBUG_CFLAGS) -o $@ tests/test_echo.c src/echo.c $(DEBUG_LDFLAGS)

# D-06 실험: SO_REUSEADDR 없이 bind만 해 본다. 서버 소스와 분리한 별도 프로그램이다(빌드만 한다).
experiment: $(EXPERIMENT)

$(EXPERIMENT): experiments/bind_no_reuse.c $(BUILD_STAMP)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) -o $@ experiments/bind_no_reuse.c $(LDFLAGS)

# 테스트용 www는 컨테이너 내부 Linux 전용 폴더에 만든다. bind mount(Docker Desktop)는 권한 000을 실제로 강제하지 못한다.
# 폴더 위치와 large.bin 크기(4MB)는 recipe 안의 고정값이다(make 변수로 덮어쓸 수 없다).
# 같은 컨테이너에서는 이 폴더를 재사용하고 아래 7개 파일만 rm -f 후 새로 쓴다(링크면 링크만 지워진다).
# 호스트 $(WWW)/는 읽기만 한다. stdout에는 폴더 경로 한 줄만 쓴다: dir=$$(make -s fixtures)
FIXTURE_COPY = index.html style.css app.js image.png photo.jpg

fixtures:
	@set -e; dir=/home/jungle/mini-webserver-www; \
	if [ "$$(id -u)" -eq 0 ]; then echo "root로는 준비하지 않는다(권한 000이 무시된다)" >&2; exit 1; fi; \
	if [ -L "$$dir" ]; then echo "$$dir 가 심볼릭 링크다. 확인 후 직접 지워라" >&2; exit 1; fi; \
	mkdir -p "$$dir"; \
	if [ -L "$$dir" ] || [ ! -d "$$dir" ]; then echo "$$dir 가 실제 디렉터리가 아니다" >&2; exit 1; fi; \
	for f in $(FIXTURE_COPY); do rm -f -- "$$dir/$$f"; cp -- "$(WWW)/$$f" "$$dir/$$f"; done; \
	rm -f -- "$$dir/large.bin"; \
	dd if=/dev/urandom of="$$dir/large.bin" bs=1048576 count=4 status=none; \
	rm -f -- "$$dir/secret.txt"; \
	echo "secret: 이 내용이 응답에 보이면 안 된다(403 검증용)." > "$$dir/secret.txt"; \
	chmod 000 "$$dir/secret.txt"; \
	if [ -r "$$dir/secret.txt" ]; then echo "secret.txt가 여전히 읽힌다" >&2; exit 1; fi; \
	echo "준비 완료 (secret.txt 권한 000, large.bin 4MB). 실행: ./webserver -p 8080 -r $$dir" >&2; \
	echo "$$dir"

clean:
	rm -f $(TARGET) $(DEBUG_TARGET) $(TEST_TARGETS) $(EXPERIMENT) $(BUILD_STAMP)
	rm -rf $(TARGET).dSYM $(DEBUG_TARGET).dSYM $(addsuffix .dSYM,$(TEST_TARGETS))

help:
	@echo "make          기본 빌드 ($(TARGET))"
	@echo "make debug    디버그 빌드 ($(DEBUG_TARGET), AddressSanitizer)"
	@echo "make test     단위 테스트 빌드 후 실행 (test_options, test_echo)"
	@echo "make experiment SO_REUSEADDR 없는 bind 실험 프로그램 빌드 ($(EXPERIMENT))"
	@echo "make fixtures 컨테이너 내부 /home/jungle/mini-webserver-www에 테스트용 www 준비, 경로를 stdout에 출력"
	@echo "make clean    빌드 산출물 삭제"
