# make         → ./webserver        (일반 빌드)
# make debug   → ./webserver-debug  (ASan 디버그 빌드)
# make www     → www/의 large.bin, secret.txt 생성
# make clean   → 빌드 결과 삭제

NAME       := webserver
DEBUG_NAME := webserver-debug

SRC_DIR := src
SRCS    := $(wildcard $(SRC_DIR)/*.c)

CFLAGS_COMMON := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -MMD -MP
CFLAGS_RELEASE := $(CFLAGS_COMMON) -O2 -g
CFLAGS_DEBUG   := $(CFLAGS_COMMON) -O0 -g -fsanitize=address -fno-omit-frame-pointer
LDFLAGS_DEBUG  := -fsanitize=address

RELEASE_OBJS := $(SRCS:$(SRC_DIR)/%.c=build/release/%.o)
DEBUG_OBJS   := $(SRCS:$(SRC_DIR)/%.c=build/debug/%.o)

.PHONY: all debug www clean

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

www:
	sh scripts/make_testfiles.sh www

clean:
	rm -rf build $(NAME) $(DEBUG_NAME)

-include $(RELEASE_OBJS:.o=.d) $(DEBUG_OBJS:.o=.d)
