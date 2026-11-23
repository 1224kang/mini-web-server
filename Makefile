CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -g -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
TARGET  := webserver
PORT    ?= 18080
SRCS    := src/main.c src/server.c src/http.c src/static_files.c
OBJS    := $(SRCS:.c=.o)

TEST_BIN := tests/test_http tests/test_static

.PHONY: all clean run asan test smoke

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c src/server.h src/http.h src/static_files.h
	$(CC) $(CFLAGS) -c -o $@ $<

# 파서 단위 테스트: 소켓 없이 src/http.c 만 링크한다.
test: $(TEST_BIN)
	./tests/test_http
	./tests/test_static

tests/test_http: tests/test_http.c src/http.c src/http.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_http.c src/http.c

tests/test_static: tests/test_static.c src/static_files.c src/static_files.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_static.c src/static_files.c

# 실제 서버를 띄워 curl 로 정적 파일 서빙을 확인한다.
smoke: $(TARGET)
	./tests/smoke_static.sh

run: $(TARGET)
	./$(TARGET) $(PORT) www

# 메모리 오류 검사용 빌드 (7단계에서 본격적으로 사용)
asan: CFLAGS += -fsanitize=address -fno-omit-frame-pointer
asan: clean $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BIN)
