# =============================================================================
# VUS 编译器构建系统 (GNU Make)
# =============================================================================

# --- 变量 ---
CC       = gcc
CFLAGS   = -Wall -Wextra -g -O2 -std=c11
SRC_DIR  = src
RT_DIR   = rt
BUILD_DIR = build
TEST_DIR = tests

# 源文件
SRCS     = $(SRC_DIR)/main.c $(SRC_DIR)/token.c $(SRC_DIR)/lexer.c \
           $(SRC_DIR)/parser.c $(SRC_DIR)/generator.c $(SRC_DIR)/config.c \
           $(SRC_DIR)/ast.c $(SRC_DIR)/vus_abi.c $(SRC_DIR)/vus_plugin.c
OBJS     = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
RT_SRC   = $(RT_DIR)/libvus_rt.c
RT_OBJ   = $(BUILD_DIR)/libvus_rt.o
RT_LIB   = $(BUILD_DIR)/libvus_rt.a

# 头文件依赖（所有 .o 都依赖这些通用头）
COMMON_H = $(SRC_DIR)/token.h $(SRC_DIR)/ast.h $(SRC_DIR)/config.h

# 各源文件对应的私有头文件
MAIN_H   = $(SRC_DIR)/../include/vus/vus.h
TOKEN_H  = $(SRC_DIR)/token.h
LEXER_H  = $(SRC_DIR)/lexer.h
PARSER_H = $(SRC_DIR)/parser.h
GEN_H    = $(SRC_DIR)/generator.h
CONFIG_H = $(SRC_DIR)/config.h
RT_H     = $(RT_DIR)/libvus_rt.h

# --- 伪目标 ---
.PHONY: all clean test run-tests run build-c build-exe install uninstall format

# --- 默认目标 ---
all: vus $(RT_LIB)

# =============================================================================
# 编译目标
# =============================================================================

# 链接编译器
vus: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm -ldl

# 编译源文件
$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c $(MAIN_H) $(COMMON_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

$(BUILD_DIR)/token.o: $(SRC_DIR)/token.c $(TOKEN_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

$(BUILD_DIR)/lexer.o: $(SRC_DIR)/lexer.c $(LEXER_H) $(TOKEN_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

$(BUILD_DIR)/parser.o: $(SRC_DIR)/parser.c $(PARSER_H) $(TOKEN_H) $(SRC_DIR)/ast.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

$(BUILD_DIR)/generator.o: $(SRC_DIR)/generator.c $(GEN_H) $(TOKEN_H) $(SRC_DIR)/ast.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

$(BUILD_DIR)/config.o: $(SRC_DIR)/config.c $(CONFIG_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

$(BUILD_DIR)/ast.o: $(SRC_DIR)/ast.c $(SRC_DIR)/ast.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

# ABI 接口
ABI_H    = $(SRC_DIR)/../include/vus/vus_abi.h
PLUGIN_H = $(SRC_DIR)/../include/vus/vus_plugin.h

$(BUILD_DIR)/vus_abi.o: $(SRC_DIR)/vus_abi.c $(ABI_H) $(GEN_H) $(PARSER_H) $(LEXER_H) $(CONFIG_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

$(BUILD_DIR)/vus_plugin.o: $(SRC_DIR)/vus_plugin.c $(PLUGIN_H) $(ABI_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

# 编译运行时库
$(RT_OBJ): $(RT_SRC) $(RT_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(RT_DIR) -c -o $@ $<

# 运行时库静态归档
$(RT_LIB): $(RT_OBJ)
	ar rcs $@ $^

# =============================================================================
# 目录创建
# =============================================================================

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# =============================================================================
# 清理
# =============================================================================

clean:
	rm -rf $(BUILD_DIR) vus

# =============================================================================
# 安装 / 卸载
# =============================================================================

install: vus
	install -m 755 vus /usr/local/bin/vus

uninstall:
	rm -f /usr/local/bin/vus

# =============================================================================
# 测试
# =============================================================================

test: all
	./vus test

run-tests: all
	cd $(TEST_DIR) && bash run_tests.sh

# =============================================================================
# 运行
# =============================================================================

run: all
	./vus run $(FILE)

build-c: all
	./vus build --c-only $(FILE)

build-exe: all
	./vus build --exe $(FILE)

# =============================================================================
# 代码格式化
# =============================================================================

format:
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i $(SRC_DIR)/*.c $(SRC_DIR)/*.h $(RT_DIR)/*.c $(RT_DIR)/*.h; \
		echo "代码格式化完成。"; \
	else \
		echo "clang-format 未安装，跳过格式化。"; \
		echo "安装: sudo apt install clang-format  (Ubuntu/Debian)"; \
		echo "      sudo pacman -S clang            (Arch)"; \
		echo "      brew install clang-format       (macOS)"; \
	fi