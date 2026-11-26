CC      := gcc
CFLAGS  := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g -Iinclude
LDFLAGS := -pthread

SRC_DIR   := src
TEST_DIR  := tests
BUILD_DIR := build

# main.c를 뺀 모듈은 테스트에서도 같이 링크함
SRCS     := $(wildcard $(SRC_DIR)/*.c)
LIB_SRCS := $(filter-out $(SRC_DIR)/main.c,$(SRCS))
OBJS     := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
LIB_OBJS := $(LIB_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

TEST_SRCS := $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS := $(TEST_SRCS:$(TEST_DIR)/%.c=$(BUILD_DIR)/%)

# DESIGN.md Step 0: ./webserver -p 8080 -r ./www
TARGET := webserver

.PHONY: all debug asan test test-debug run www clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_%: $(TEST_DIR)/test_%.c $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $@

# 디버그 빌드: ASan + UBSan (DESIGN.md Step 0). 일반 빌드와 섞이지 않게 clean 후 사용
debug: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
debug: LDFLAGS += -fsanitize=address,undefined
debug: all

asan: debug

# 단위 테스트도 ASan·UBSan을 켜고 빌드해서 실행. 일반 빌드와 섞이지 않게 clean 후 사용
test-debug: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
test-debug: LDFLAGS += -fsanitize=address,undefined
test-debug: test

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== $$t"; ./$$t || exit 1; done

run: all
	./$(TARGET) -p 8080 -r ./www

# 커밋하지 않는 테스트 파일(large.bin, secret.txt) 만들기
www:
	./scripts/make_www.sh

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(OBJS:.o=.d)
