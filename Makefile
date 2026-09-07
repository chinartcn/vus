# =============================================================================
# VUS 编译器构建系统 (GNU Make)
# =============================================================================

# --- 变量 ---
CC       = gcc
CXX      = g++
CFLAGS   = -Wall -Wextra -g -O2 -std=c11 -Wno-format-truncation $(VERSION_DEF)
SRC_DIR  = src
RT_DIR   = rt
BUILD_DIR = build
TEST_DIR = tests

# 版本号（发布/安装/帮助均使用；可 make VUS_VERSION=xxx 覆盖）
VUS_VERSION ?= 3.0.20260904150204
VERSION_DEF = -DVUS_VERSION_STR=\"$(VUS_VERSION)\"

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
           $(SRC_DIR)/vus_chart.c $(SRC_DIR)/vus_vaz.c \
           $(SRC_DIR)/vua_lint.c \
           $(SRC_DIR)/lsp/lsp.c $(SRC_DIR)/lsp/vus_builtin.c
OBJS     = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
RT_SRC   = $(RT_DIR)/libvus_rt.c
RT_CORO  = $(RT_DIR)/vus_coro.c
RT_C_IMPL = $(RT_DIR)/vus_rt_c_impl.c
RT_OBJ   = $(BUILD_DIR)/libvus_rt.o
RT_CORO_OBJ = $(BUILD_DIR)/vus_coro.o
RT_C_IMPL_OBJ = $(BUILD_DIR)/vus_rt_c_impl.o
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
GUI_SRC = $(RT_DIR)/guilite_bridge.c $(RT_DIR)/guilite_platform.c $(RT_DIR)/guilite_wrapper.cpp $(RT_DIR)/gifdec/gifdec.c
GUI_OBJ = $(BUILD_DIR)/guilite_bridge.o $(BUILD_DIR)/guilite_platform.o $(BUILD_DIR)/guilite_wrapper.o $(BUILD_DIR)/gifdec.o
GUI_INC = -I$(RT_DIR) -I$(GUI_DIR)

# VUS XYZ 体感音游运行时（传感器/时钟/音频控制，并入运行时静态库）
XYZ_SRC = $(RT_DIR)/vus_xyz.c
XYZ_OBJ = $(BUILD_DIR)/vus_xyz.o

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

# VUA 界面运行时（native 组件流：解析/严格校验/渲染树归一/事件派发）
VUA_SRC  = $(RT_DIR)/vua.c
VUA_OBJ  = $(BUILD_DIR)/vua.o

# ---- 扩展内建/m 依赖的对象（须在 vus 规则之前定义：依赖列表解析期展开） ----
# miniz：（解压_zip/压缩_zip 与 VAZ 解包，MIT）— O0 最快编译。
# 3.0.2 拆分为 4 个实现 TU：miniz.c(核心) + miniz_tdef.c(deflate) +
# miniz_tinfl.c(inflate) + miniz_zip.c(zip)，各自编译后并入静态库。
MINIZ_SRC = $(RT_DIR)/miniz/miniz.c $(RT_DIR)/miniz/miniz_tdef.c \
            $(RT_DIR)/miniz/miniz_tinfl.c $(RT_DIR)/miniz/miniz_zip.c
MINIZ_OBJ = $(BUILD_DIR)/miniz.o $(BUILD_DIR)/miniz_tdef.o \
            $(BUILD_DIR)/miniz_tinfl.o $(BUILD_DIR)/miniz_zip.o

# Oniguruma：（正则内建，BSD-2）— 核心 + UTF-8 编码，gnu11（xalloca 走 GCC 内置）
ONIG_SRC = $(RT_DIR)/oniguruma/regcomp.c $(RT_DIR)/oniguruma/regenc.c \
           $(RT_DIR)/oniguruma/regerror.c $(RT_DIR)/oniguruma/regext.c \
           $(RT_DIR)/oniguruma/regparse.c $(RT_DIR)/oniguruma/regsyntax.c \
           $(RT_DIR)/oniguruma/regtrav.c $(RT_DIR)/oniguruma/regversion.c \
           $(RT_DIR)/oniguruma/regexec.c $(RT_DIR)/oniguruma/st.c \
           $(RT_DIR)/oniguruma/onig_init.c $(RT_DIR)/oniguruma/unicode.c \
           $(RT_DIR)/oniguruma/utf8.c $(RT_DIR)/oniguruma/ascii.c \
           $(RT_DIR)/oniguruma/unicode_fold1_key.c \
           $(RT_DIR)/oniguruma/unicode_fold2_key.c \
           $(RT_DIR)/oniguruma/unicode_fold3_key.c \
           $(RT_DIR)/oniguruma/unicode_unfold_key.c
ONIG_OBJ = $(ONIG_SRC:$(RT_DIR)/oniguruma/%.c=$(BUILD_DIR)/onig_%.o)

# 扩展内建（哈希/ZIP/图片导出/正则，均 vendored 进 rt/）
VUS_EXT_SRC = $(RT_DIR)/vus_hash.c $(RT_DIR)/vus_zip.c \
              $(RT_DIR)/vus_img.c $(RT_DIR)/vus_regex.c
VUS_EXT_OBJ = $(VUS_EXT_SRC:$(RT_DIR)/%.c=$(BUILD_DIR)/%.o)

# 链接编译器（含 yyjson、VUA 运行时与其所需 libvus_rt/协程/EasyLogger：
# CLI `vus lint` 与 LSP .vua 校验闭环进程内复用 vua.c 严格校验 + 渲染树归一）
# 注：Oniguruma/扩展内建/miniz 对象同时被 libvus_rt.a 与 vus 引用，必须同时
# 列入 vus 的前置依赖，否则 `make vus`/`make` 单独构建时缺对象链接失败。
vus: $(OBJS) $(YYJSON_OBJ) $(VUA_OBJ) $(RT_OBJ) $(RT_CORO_OBJ) $(RT_C_IMPL_OBJ) $(EL_OBJ) \
      $(ONIG_OBJ) $(VUS_EXT_OBJ) $(MINIZ_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lm -ldl -lpthread

# 编译源文件
$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c $(MAIN_H) $(COMMON_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -I$(RT_DIR) -c -o $@ $<

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

# .vua 离线校验（vus lint）/ LSP .vua 诊断（复用 rt/vua.c 严格校验+渲染树归一）
VUA_LINT_H = $(SRC_DIR)/vua_lint.h

$(BUILD_DIR)/vua_lint.o: $(SRC_DIR)/vua_lint.c $(VUA_LINT_H) $(RT_DIR)/vua.h $(RT_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -I$(RT_DIR) -c -o $@ $<

$(VUA_OBJ): $(RT_DIR)/vua.c $(RT_DIR)/vua.h $(RT_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(RT_DIR) -c -o $@ $<

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

# 谱面生成（体感音游）
CHART_H = $(SRC_DIR)/vus_chart.h

$(BUILD_DIR)/vus_chart.o: $(SRC_DIR)/vus_chart.c $(CHART_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

# vaz Android 扩展包（控件模板展开 + 逻辑库依赖导入）
VAZ_INT = $(SRC_DIR)/vus_vaz.h

$(BUILD_DIR)/vus_vaz.o: $(SRC_DIR)/vus_vaz.c $(VAZ_INT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -I$(RT_DIR) -I$(RT_DIR)/yyjson -c -o $@ $<

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

# 编译 FFI Bridge C 域实现（dlopen 装载，随运行时库归档）
$(RT_C_IMPL_OBJ): $(RT_C_IMPL) $(RT_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(RT_DIR) -c -o $@ $<

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

# 编译 gifdec（CC0 单驱动 GIF 解码库，随 GUI 桥接链入静态库）
$(BUILD_DIR)/gifdec.o: $(RT_DIR)/gifdec/gifdec.c $(RT_DIR)/gifdec/gifdec.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(RT_DIR) -c -o $@ $<

# 编译 VUS XYZ 体感音游运行时（传感器/时钟/音频控制）
$(XYZ_OBJ): $(XYZ_SRC) $(RT_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(RT_DIR) -c -o $@ $<

# 编译 yyjson 单文件 JSON 库（纯 C，并入运行时静态库）
# 说明：yyjson 单文件体积大，-O2 优化编译很慢；改为 -O0（最快编译）。
#       该库仅做 JSON 解析/生成，对整体运行性能影响可忽略；如仍嫌慢可再提
#       到 -O1，需要运行时极致性能时才改回 -O2。
$(YYJSON_OBJ): $(YYJSON_SRC) | $(BUILD_DIR)
	$(CC) -Wall -Wextra -O0 -std=c11 $(YYJSON_INC) -c -o $@ $<

# miniz 编译规则（变量定义见 vus 规则前的 MINIZ_SRC/MINIZ_OBJ）
$(BUILD_DIR)/miniz.o: $(RT_DIR)/miniz/miniz.c $(RT_DIR)/miniz/miniz.h $(RT_DIR)/miniz/miniz_export.h | $(BUILD_DIR)
	$(CC) -Wall -Wextra -O0 -std=c11 -I$(RT_DIR) -c -o $@ $<
$(BUILD_DIR)/miniz_tdef.o: $(RT_DIR)/miniz/miniz_tdef.c $(RT_DIR)/miniz/miniz_tdef.h | $(BUILD_DIR)
	$(CC) -Wall -Wextra -O0 -std=c11 -I$(RT_DIR) -c -o $@ $<
$(BUILD_DIR)/miniz_tinfl.o: $(RT_DIR)/miniz/miniz_tinfl.c $(RT_DIR)/miniz/miniz_tinfl.h | $(BUILD_DIR)
	$(CC) -Wall -Wextra -O0 -std=c11 -I$(RT_DIR) -c -o $@ $<
$(BUILD_DIR)/miniz_zip.o: $(RT_DIR)/miniz/miniz_zip.c $(RT_DIR)/miniz/miniz_zip.h | $(BUILD_DIR)
	$(CC) -Wall -Wextra -O0 -std=c11 -I$(RT_DIR) -c -o $@ $<

# Oniguruma 编译规则（变量定义见 vus 规则前的 ONIG_SRC/ONIG_OBJ）
$(BUILD_DIR)/onig_%.o: $(RT_DIR)/oniguruma/%.c $(RT_DIR)/oniguruma/oniguruma.h | $(BUILD_DIR)
	$(CC) -Wall -Wextra -O0 -std=gnu11 -Wno-unused-parameter -Wno-sign-compare \
		-I$(RT_DIR)/oniguruma -c -o $@ $<

# VUS 扩展内建模块编译规则（哈希/ZIP/图片/正则的 VusString* 封装，随运行时库编译；
# 变量定义见 vus 规则前的 VUS_EXT_SRC/VUS_EXT_OBJ）
$(BUILD_DIR)/vus_hash.o: $(RT_DIR)/vus_hash.c $(RT_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(RT_DIR) -c -o $@ $<
$(BUILD_DIR)/vus_zip.o: $(RT_DIR)/vus_zip.c $(RT_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(RT_DIR) -I$(RT_DIR)/stb -c -o $@ $<
$(BUILD_DIR)/vus_img.o: $(RT_DIR)/vus_img.c $(RT_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(RT_DIR) -I$(RT_DIR)/stb -c -o $@ $<
$(BUILD_DIR)/vus_regex.o: $(RT_DIR)/vus_regex.c $(RT_H) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(RT_DIR) -I$(RT_DIR)/oniguruma -c -o $@ $<

# 运行时库静态归档（含 vus_coro.o、yyjson、easylogger elog.o 与 GuiLite 图形库）
# 顺序注意：GNU ld 对归档只做单遍扫描，被依赖对象须排在其引用者之后——
# oniguruma 排在 vus_regex 前；miniz 排在最后（vus_zip 引用 mz_*）。
$(RT_LIB): $(RT_OBJ) $(RT_CORO_OBJ) $(RT_C_IMPL_OBJ) $(YYJSON_OBJ) $(EL_OBJ) $(GUI_OBJ) $(XYZ_OBJ) $(GLES_ARCHIVE_OBJ) \
           $(ONIG_OBJ) $(VUS_EXT_OBJ) $(MINIZ_OBJ)
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

install: all
	install -m 755 vus /usr/local/bin/vus
	install -d /usr/local/share/vus/scripts
	install -m 644 scripts/vux_plugin_manager.py /usr/local/share/vus/scripts/
	install -m 644 scripts/vux_plugin_entry.py /usr/local/share/vus/scripts/
	install -m 644 scripts/gen_jni_bridge.py /usr/local/share/vus/scripts/
	install -d /usr/local/share/vus/examples
	install -m 644 examples/hello.vus /usr/local/share/vus/examples/
	install -d /usr/local/share/vus/include/vus
	install -m 644 include/vus/vus.h /usr/local/share/vus/include/vus/
	install -m 644 include/vus/vus_abi.h /usr/local/share/vus/include/vus/
	install -m 644 include/vus/vus_plugin.h /usr/local/share/vus/include/vus/
	install -m 644 include/vus/vus_lang.h /usr/local/share/vus/include/vus/
	install -m 644 include/vus/vus_vusx.h /usr/local/share/vus/include/vus/
	install -d /usr/local/share/vus/rt
	install -m 644 rt/libvus_rt.h /usr/local/share/vus/rt/
	install -m 644 build/libvus_rt.a /usr/local/share/vus/rt/

uninstall:
	rm -f /usr/local/bin/vus
	rm -rf /usr/local/share/vus

# =============================================================================
# 测试
# =============================================================================

test: all
	./vus test

# FFI Bridge C 域示例插件（test_ext_c.vus 依赖；构建产物不入库不安装）
EXT_C_PLUGIN_DIR = examples/ext_c_plugin
EXT_C_PLUGIN_SO  = $(EXT_C_PLUGIN_DIR)/c_math.so
$(EXT_C_PLUGIN_SO): $(EXT_C_PLUGIN_DIR)/c_math.c include/vus/vus_rt_bridge.h
	$(CC) -shared -fPIC -Iinclude/vus -o $@ $<

run-tests: all $(EXT_C_PLUGIN_SO)
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