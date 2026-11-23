# make         → ./webserver        (일반 빌드)
# make debug   → ./webserver-debug  (ASan 디버그 빌드)
# make test    → 단위 테스트(파서, MIME) 빌드 후 실행 (ASan 포함)
# make www     → www/의 large.bin, secret.txt 생성
# make clean   → 빌드 결과 삭제

NAME       := webserver
DEBUG_NAME := webserver-debug

SRC_DIR := src
SRCS    := $(wildcard $(SRC_DIR)/*.c)

CFLAGS_COMMON := -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -MMD -MP
CFLAGS_RELEASE := $(CFLAGS_COMMON) -O2 -g
CFLAGS_DEBUG   := $(CFLAGS_COMMON) -O0 -g -fsanitize=address -fno-omit-frame-pointer
LDFLAGS_DEBUG  := -fsanitize=address

RELEASE_OBJS := $(SRCS:$(SRC_DIR)/%.c=build/release/%.o)
DEBUG_OBJS   := $(SRCS:$(SRC_DIR)/%.c=build/debug/%.o)

# 단위 테스트: tests/test_X.c는 소켓을 쓰지 않는 src/X.c 하나만 링크한다
TESTS := build/test/test_request build/test/test_mime
CFLAGS_TEST := -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic \
               -O0 -g -fsanitize=address -fno-omit-frame-pointer -I$(SRC_DIR)

.PHONY: all debug test www clean

all: $(NAME)

debug: $(DEBUG_NAME)

$(NAME): $(RELEASE_OBJS)
	$(CC) $(CFLAGS_RELEASE) -o $@ $^

$(DEBUG_NAME): $(DEBUG_OBJS)
	$(CC) $(CFLAGS_DEBUG) $(LDFLAGS_DEBUG) -o $@ $^

build/release/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RELEASE) -c $< -o $@

build/debug/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DEBUG) -c $< -o $@

# 하나가 실패해도 나머지를 다 돌리고, 실패가 있으면 종료 코드 1
test: $(TESTS)
	@status=0; for t in $(TESTS); do ./$$t || status=1; done; exit $$status

build/test/test_%: tests/test_%.c $(SRC_DIR)/%.c $(SRC_DIR)/%.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TEST) -o $@ tests/test_$*.c $(SRC_DIR)/$*.c

www:
	sh scripts/make_testfiles.sh www

clean:
	rm -rf build $(NAME) $(DEBUG_NAME)

-include $(RELEASE_OBJS:.o=.d) $(DEBUG_OBJS:.o=.d)
