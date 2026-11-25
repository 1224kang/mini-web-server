CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -g -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
TARGET  := webserver
PORT    ?= 18080
SRCS    := src/main.c src/server.c src/http.c src/static_files.c src/conn.c \
           src/config.c src/cgi.c src/upstream.c src/builtin.c
OBJS    := $(SRCS:.c=.o)

TEST_BIN := tests/test_http tests/test_static tests/test_dynamic

.PHONY: all clean run asan test smoke smoke-concurrency smoke-dynamic

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

HDRS    := $(wildcard src/*.h)

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# 파서 단위 테스트: 소켓 없이 src/http.c 만 링크한다.
test: $(TEST_BIN)
	./tests/test_http
	./tests/test_static
	./tests/test_dynamic

tests/test_http: tests/test_http.c src/http.c src/http.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_http.c src/http.c

tests/test_static: tests/test_static.c src/static_files.c src/static_files.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_static.c src/static_files.c

tests/test_dynamic: tests/test_dynamic.c src/upstream.c src/builtin.c src/config.c src/http.c $(HDRS)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_dynamic.c src/upstream.c src/builtin.c src/config.c src/http.c

# 실제 서버를 띄워 curl 로 정적 파일 서빙을 확인한다.
smoke: $(TARGET)
	./tests/smoke_static.sh

# 4단계: 동시 접속, keep-alive, 타임아웃, 503, 시그널 종료 (타임아웃을 기다리느라 ~15초 걸림)
smoke-concurrency: $(TARGET)
	python3 tests/smoke_concurrency.py

# 5단계: CGI, 내장 핸들러
smoke-dynamic: $(TARGET)
	python3 tests/smoke_dynamic.py

run: $(TARGET)
	./$(TARGET) $(PORT) www

# 메모리 오류 검사용 빌드 (7단계에서 본격적으로 사용)
asan: CFLAGS += -fsanitize=address -fno-omit-frame-pointer
asan: clean $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BIN)
