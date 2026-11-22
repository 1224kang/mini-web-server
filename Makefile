CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -g -D_POSIX_C_SOURCE=200809L
TARGET  := webserver
PORT    ?= 18080
SRCS    := src/main.c src/server.c src/http.c
OBJS    := $(SRCS:.c=.o)

TEST_BIN := tests/test_http

.PHONY: all clean run asan test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c src/server.h src/http.h
	$(CC) $(CFLAGS) -c -o $@ $<

# 파서 단위 테스트: 소켓 없이 src/http.c 만 링크한다.
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): tests/test_http.c src/http.c src/http.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_http.c src/http.c

run: $(TARGET)
	./$(TARGET) $(PORT)

# 메모리 오류 검사용 빌드 (7단계에서 본격적으로 사용)
asan: CFLAGS += -fsanitize=address -fno-omit-frame-pointer
asan: clean $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BIN)
