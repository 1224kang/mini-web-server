# mini-web-server Makefile
#
#   make            : 일반(release) 빌드             -> ./mini_web_server
#   make asan       : AddressSanitizer(+Leak) 빌드   -> ./mini_web_server_asan
#   make test       : 단위 테스트 빌드 + 실행          (build/release/test_parser)
#   make test-asan  : ASan 으로 단위 테스트 빌드 + 실행 (build/asan/test_parser)
#   make valgrind   : valgrind 로 단위 테스트 메모리 검사 (valgrind 설치 필요)
#   make e2e        : ASan 서버를 띄워 end-to-end 테스트 (curl, python3 필요)
#   make clean      : 빌드 산출물 삭제
#
# 빌드 모드별로 오브젝트 디렉토리(build/<mode>)를 분리해
# 일반 빌드와 asan 빌드의 .o 파일이 섞이지 않도록 한다.

CC      ?= cc
MODE    ?= release

CFLAGS  := -std=gnu11 -Wall -Wextra -pthread -Iinclude
LDFLAGS := -pthread

ifeq ($(MODE),asan)
  # LeakSanitizer 는 Linux 에서 ASan 에 포함되어 기본 활성화된다.
  # (macOS 기본 clang 은 -fsanitize=leak 단독 지정을 지원하지 않으므로 address 만 지정)
  CFLAGS  += -O1 -g -fno-omit-frame-pointer -fsanitize=address
  LDFLAGS += -fsanitize=address
  TARGET  := mini_web_server_asan
else
  CFLAGS  += -O2 -g
  TARGET  := mini_web_server
endif

SRC_DIR  := src
TEST_DIR := tests
OBJ_DIR  := build/$(MODE)

SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
LIB_OBJS := $(filter-out $(OBJ_DIR)/main.o,$(OBJS))     # 테스트에 링크할 때는 main 제외
TEST_BIN := $(OBJ_DIR)/test_parser
DEPS     := $(OBJS:.o=.d) $(OBJ_DIR)/test_parser.d

.PHONY: all asan test test-asan valgrind e2e clean

all: $(TARGET)

asan:
	$(MAKE) MODE=asan

test: $(TEST_BIN)
	./$(TEST_BIN)

test-asan:
	$(MAKE) MODE=asan test

valgrind: $(TEST_BIN)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TEST_BIN)

e2e:
	$(MAKE) MODE=asan
	bash $(TEST_DIR)/e2e_static.sh ./mini_web_server_asan
	bash $(TEST_DIR)/e2e_server.sh ./mini_web_server_asan

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_BIN): $(OBJ_DIR)/test_parser.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/test_parser.o: $(TEST_DIR)/test_parser.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf build mini_web_server mini_web_server_asan

-include $(DEPS)
