CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -g -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
MODE    ?= release
PORT    ?= 18080
SRCS    := src/main.c src/server.c src/http.c src/static_files.c src/conn.c \
           src/config.c src/cgi.c src/upstream.c src/builtin.c
OBJ_DIR := build/$(MODE)
OBJS    := $(SRCS:src/%.c=$(OBJ_DIR)/%.o)

ifeq ($(MODE),asan)
CFLAGS   += -fsanitize=address -fno-omit-frame-pointer
TARGET   := webserver_asan
TEST_DIR := $(OBJ_DIR)
else
TARGET   := webserver
TEST_DIR := tests
endif

TEST_BIN := $(addprefix $(TEST_DIR)/,test_http test_static test_dynamic)

.PHONY: all clean run run-proxy backends asan test test-asan smoke smoke-concurrency smoke-dynamic

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

HDRS    := $(wildcard src/*.h)

$(OBJ_DIR)/%.o: src/%.c $(HDRS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $@

# 각 빌드 모드의 단위 테스트 실행 파일을 사용한다.
test: $(TEST_BIN)
	./$(TEST_DIR)/test_http
	./$(TEST_DIR)/test_static
	./$(TEST_DIR)/test_dynamic

$(TEST_DIR)/test_http: tests/test_http.c src/http.c src/http.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -Isrc -o $@ $(filter %.c,$^)

$(TEST_DIR)/test_static: tests/test_static.c src/static_files.c src/static_files.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -Isrc -o $@ $(filter %.c,$^)

$(TEST_DIR)/test_dynamic: tests/test_dynamic.c src/upstream.c src/builtin.c src/config.c src/http.c $(HDRS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -Isrc -o $@ $(filter %.c,$^)

# 실제 서버를 띄워 curl 로 정적 파일 서빙을 확인한다.
smoke: $(TARGET)
	./tests/smoke_static.sh

# 4단계: 동시 접속, keep-alive, 타임아웃, 503, 시그널 종료 (타임아웃을 기다리느라 ~15초 걸림)
smoke-concurrency: $(TARGET)
	python3 tests/smoke_concurrency.py

# 5·6단계: CGI, 내장 핸들러, 리버스 프록시 (백엔드를 띄웠다 죽였다 함)
smoke-dynamic: $(TARGET)
	python3 tests/smoke_dynamic.py

run: $(TARGET)
	./$(TARGET) $(PORT) www

# 6단계 데모: 다른 터미널에서 make backends 를 먼저 띄운다
run-proxy: $(TARGET)
	./$(TARGET) -c conf/proxy.conf $(PORT) www

# 테스트용 백엔드 3개 (Ctrl+C 로 같이 종료)
backends:
	trap 'kill 0' INT TERM; \
	python3 tests/backend.py 8001 & python3 tests/backend.py 8002 & python3 tests/backend.py 8003 & \
	wait

asan:
	$(MAKE) MODE=asan all

test-asan:
	$(MAKE) MODE=asan test

clean:
	rm -f $(SRCS:.c=.o) webserver webserver_asan tests/test_http tests/test_static tests/test_dynamic
	rm -rf build/release build/asan
