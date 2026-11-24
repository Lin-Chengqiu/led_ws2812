# ws2812 - 主机端构建
#
#   make        编译单元测试
#   make test   编译并运行
#   make clean  清理

CC       ?= cc
CFLAGS   ?= -std=c99 -Wall -Wextra -O2
CPPFLAGS += -I.

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

LIB_SRC  := ws2812.c
TEST_SRC := tests/test_ws2812.c
TEST_BIN := tests/test_ws2812$(EXE)

.PHONY: all test clean

all: $(TEST_BIN)

$(TEST_BIN): $(LIB_SRC) $(TEST_SRC) ws2812.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LIB_SRC) $(TEST_SRC) -o $@

test: $(TEST_BIN)
	@./$(TEST_BIN)

clean:
	rm -f tests/test_ws2812 tests/test_ws2812.exe
