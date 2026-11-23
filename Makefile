# mini-web-server Makefile
#
#   make          : 일반(release) 빌드  -> ./mini_web_server
#   make asan     : AddressSanitizer 디버그 빌드 -> ./mini_web_server_asan
#   make clean    : 빌드 산출물 삭제
#
# 빌드 모드별로 오브젝트 디렉토리(build/<mode>)를 분리해
# 일반 빌드와 asan 빌드의 .o 파일이 섞이지 않도록 한다.

CC      ?= cc
MODE    ?= release

CFLAGS  := -std=gnu11 -Wall -Wextra -pthread -Iinclude
LDFLAGS := -pthread

ifeq ($(MODE),asan)
  CFLAGS  += -O1 -g -fno-omit-frame-pointer -fsanitize=address
  LDFLAGS += -fsanitize=address
  TARGET  := mini_web_server_asan
else
  CFLAGS  += -O2 -g
  TARGET  := mini_web_server
endif

SRC_DIR := src
OBJ_DIR := build/$(MODE)
SRCS    := $(wildcard $(SRC_DIR)/*.c)
OBJS    := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
DEPS    := $(OBJS:.o=.d)

.PHONY: all asan clean

all: $(TARGET)

asan:
	$(MAKE) MODE=asan

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf build mini_web_server mini_web_server_asan

-include $(DEPS)
