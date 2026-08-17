# =============================================================================
# VUS 编译器构建系统 (GNU Make)
# =============================================================================

# --- 变量 ---
CC       = gcc
CXX      = g++
CFLAGS   = -Wall -Wextra -g -O2 -std=c11 -Wno-format-truncation
SRC_DIR  = src
RT_DIR   = rt
BUILD_DIR = build
TEST_DIR = tests

# libpython 检测（可选）：存在则启用进程内嵌入，否则降级子进程
PY_INC := $(shell python3-config --includes 2>/dev/null)
PY_LD  := $(shell python3-config --ldflags 2>/dev/null)
PY_VER := $(strip $(shell python3 -c "import sys;print('libpython%d.%d.so'%(sys.version_info[0],sys.version_info[1]))" 2>/dev/null))
ifeq ($(strip $(PY_INC)),)
PY_DEF =
else
PY_DEF = -DVUS_USE_PY
ifneq ($(strip $(PY_VER)),)
# 注入与编译环境匹配的 libpython soname，避免运行时 dlopen 硬编码版本
PY_DEF += -DVUS_PY_SONAME=\"$(PY_VER)\"
endif
endif

# 源文件
SRCS     = $(SRC_DIR)/main.c $(SRC_DIR)/token.c $(SRC_DIR)/lexer.c \
           $(SRC_DIR)/parser.c $(SRC_DIR)/generator.c $(SRC_DIR)/config.c \
           $(SRC_DIR)/ast.c $(SRC_DIR)/vus_abi.c $(SRC_DIR)/vus_plugin.c \
           $(SRC_DIR)/vus_lang.c $(SRC_DIR)/vus_vusx.c $(SRC_DIR)/vus_apk.c \
           $(SRC_DIR)/lsp/lsp.c $(SRC_DIR)/lsp/vus_builtin.c
OBJS     = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
RT_SRC   = $(RT_DIR)/libvus_rt.c
RT_CORO  = $(RT_DIR)/vus_coro.c
RT_OBJ   = $(BUILD_DIR)/libvus_rt.o
RT_CORO_OBJ = $(BUILD_DIR)/vus_coro.o
RT_LIB   = $(BUILD_DIR)/libvus_rt.a

# yyjson 单头/源：纯 C JSON 解析/生成（并入运行时静态库）
YYJSON_SRC = $(RT_DIR)/yyjson/yyjson.c
YYJSON_OBJ = $(BUILD_DIR)/yyjson.o
YYJSON_INC = -I$(RT_DIR)

# EasyLogger 日志库（VUS 静态集成）
EL_DIR = $(RT_DIR)/easylogger
EL_SRC = $(EL_DIR)/src/elog.c $(EL_DIR)/src/elog_utils.c $(RT_DIR)/elog_port.c
EL_OBJ = $(BUILD_DIR)/elog.o $(BUILD_DIR)/elog_utils.o $(BUILD_DIR)/elog_port.o
EL_INC = -I$(EL_DIR)/inc

# GuiLite 图形库（VUS GUI 集成）：C++ 包装 + C 桥接 + C 平台层
GUI_DIR = $(RT_DIR)/guilite
GUI_SRC = $(RT_DIR)/guilite_bridge.c $(RT_DIR)/guilite_platform.c $(RT_DIR)/guilite_wrapper.cpp
GUI_OBJ = $(BUILD_DIR)/guilite_bridge.o $(BUILD_DIR)/guilite_platform.o $(BUILD_DIR)/guilite_wrapper.o
GUI_INC = -I$(RT_DIR) -I$(GUI_DIR)

# EGL + OpenGL ES 底层 GPU 上屏（可选，默认关闭）：
#   启用：make VUS_GUI_GLES=1
#   运行：VUS_GUI_GLES=1 ./vus run <脚本>
#   使 redraw 走纹理上屏(GL)，替换逐像素 XPutImage；EGL 初始化失败自动回退软渲染。
GLES_SRC = $(RT_DIR)/guilite_gles.c
GLES_OBJ = $(BUILD_DIR)/guilite_gles.o
ifdef VUS_GUI_GLES
GLES_DEF = -DVUS_GUI_GLES
GLES_LIBS = -lEGL -lGLESv2
GLES_ARCHIVE_OBJ = $(GLES_OBJ)
else
GLES_DEF =
GLES_LIBS =
GLES_ARCHIVE_OBJ =
endif

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

# 链接编译器（含 yyjson：LSP 服务器模块在进程内直接使用该 JSON 库）
vus: $(OBJS) $(YYJSON_OBJ)
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

# 语言插件系统
LANG_H   = $(SRC_DIR)/../include/vus/vus_lang.h
LANG_INT = $(SRC_DIR)/vus_lang.h

$(BUILD_DIR)/vus_lang.o: $(SRC_DIR)/vus_lang.c $(LANG_H) $(LANG_INT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

# vusx 插件系统
VUSX_H = $(SRC_DIR)/../include/vus/vus_vusx.h
VUSX_INT = $(SRC_DIR)/vus_vusx.h

$(BUILD_DIR)/vus_vusx.o: $(SRC_DIR)/vus_vusx.c $(VUSX_H) $(VUSX_INT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

# APK 打包
APK_H = $(SRC_DIR)/vus_apk.h

$(BUILD_DIR)/vus_apk.o: $(SRC_DIR)/vus_apk.c $(APK_H) $(GEN_H) $(CONFIG_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

# 语言服务器（LSP）
LSP_H     = $(SRC_DIR)/lsp/lsp.h
BUILTIN_H = $(SRC_DIR)/lsp/vus_builtin.h

$(BUILD_DIR)/lsp:
	mkdir -p $@

$(BUILD_DIR)/lsp/vus_builtin.o: $(SRC_DIR)/lsp/vus_builtin.c $(BUILTIN_H) | $(BUILD_DIR)/lsp
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

$(BUILD_DIR)/lsp/lsp.o: $(SRC_DIR)/lsp/lsp.c $(LSP_H) $(BUILTIN_H) | $(BUILD_DIR)/lsp
	$(CC) $(CFLAGS) -I$(SRC_DIR) $(YYJSON_INC) -c -o $@ $<

# 编译运行时库（启用进程内嵌入时追加 libpython 头/链接参数）
$(RT_OBJ): $(RT_SRC) $(RT_H) $(RT_DIR)/vus_coro.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(PY_DEF) $(PY_INC) -I$(RT_DIR) $(EL_INC) -c -o $@ $<

# 编译协程模块（独立，避免 libvus_rt.c 里做 inline asm 时跟 C11 冲突）
$(RT_CORO_OBJ): $(RT_CORO) $(RT_DIR)/vus_coro.h | $(BUILD_DIR)
	$(CC) -Wall -Wextra -g -O2 -Wno-format-truncation -I$(RT_DIR) -c -o $@ $<

# 编译 EasyLogger 核心源码
$(BUILD_DIR)/elog.o: $(EL_DIR)/src/elog.c $(EL_DIR)/inc/elog.h $(EL_DIR)/inc/elog_cfg.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EL_INC) -c -o $@ $<

$(BUILD_DIR)/elog_utils.o: $(EL_DIR)/src/elog_utils.c $(EL_DIR)/inc/elog.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EL_INC) -c -o $@ $<

$(BUILD_DIR)/elog_port.o: $(RT_DIR)/elog_port.c $(EL_DIR)/inc/elog.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(EL_INC) -I$(RT_DIR) -c -o $@ $<

# 编译 GuiLite 图形库（C 桥接 / C 平台 / C++ 包装）
$(BUILD_DIR)/guilite_bridge.o: $(RT_DIR)/guilite_bridge.c $(RT_DIR)/guilite_bridge.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(PY_DEF) $(PY_INC) -I$(RT_DIR) $(shell pkg-config --cflags freetype2 2>/dev/null) -DVUS_GUI_X11 $(GLES_DEF) -c -o $@ $<

$(BUILD_DIR)/guilite_platform.o: $(RT_DIR)/guilite_platform.c $(RT_DIR)/guilite_bridge.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(PY_DEF) $(PY_INC) -I$(RT_DIR) $(shell pkg-config --cflags freetype2 2>/dev/null) -DVUS_GUI_X11 $(GLES_DEF) -c -o $@ $<

$(BUILD_DIR)/guilite_gles.o: $(RT_DIR)/guilite_gles.c $(RT_DIR)/guilite_gles.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(PY_DEF) $(PY_INC) -I$(RT_DIR) -DVUS_GUI_X11 -DVUS_GUI_GLES -c -o $@ $<

$(BUILD_DIR)/guilite_wrapper.o: $(RT_DIR)/guilite_wrapper.cpp $(GUI_DIR)/GuiLite.h | $(BUILD_DIR)
	$(CXX) -Wall -Wextra -g -O2 $(GUI_INC) -c -o $@ $<

# 编译 yyjson 单文件 JSON 库（纯 C，并入运行时静态库）
$(YYJSON_OBJ): $(YYJSON_SRC) | $(BUILD_DIR)
	$(CC) -Wall -Wextra -O2 -std=c11 $(YYJSON_INC) -c -o $@ $<

# 运行时库静态归档（含 vus_coro.o、yyjson、easylogger elog.o 与 GuiLite 图形库）
$(RT_LIB): $(RT_OBJ) $(RT_CORO_OBJ) $(YYJSON_OBJ) $(EL_OBJ) $(GUI_OBJ) $(GLES_ARCHIVE_OBJ)
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
	install -d /usr/local/share/vus/scripts
	install -m 644 scripts/vux_plugin_manager.py /usr/local/share/vus/scripts/
	install -m 644 scripts/vux_plugin_entry.py /usr/local/share/vus/scripts/
	install -d /usr/local/share/vus/examples/plugins
	cp -r examples/plugins/* /usr/local/share/vus/examples/plugins/ 2>/dev/null || true
	install -d /usr/local/share/vus/include/vus
	install -m 644 include/vus/vus_lang.h /usr/local/share/vus/include/vus/
	install -m 644 include/vus/vus_vusx.h /usr/local/share/vus/include/vus/

uninstall:
	rm -f /usr/local/bin/vus
	rm -rf /usr/local/share/vus

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