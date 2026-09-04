#!/bin/bash
# VUS 预编译包构建脚本
# 用法: bash scripts/build_release.sh [版本号]
# 依赖: gcc, make（本机构建）；交叉编译需 gcc-aarch64-linux-gnu / gcc-arm-linux-gnueabihf
# 产物: release/vus-<arch>.tar.gz（vus-<arch>/ 目录 = install.sh 期待的解压形态）
#
# 与 Makefile 保持同构：
#   - 编译器含全部子命令（含 chart/vaz/lsp）
#   - 运行时完整归档（协程/yyjson/EasyLogger/GuiLite(可选)/vus_xyz）
#   - 包内附带 rt/, scripts/, include/vus/, examples/, tests/（install.sh 一键测试需要）

set -e

VERSION="${1:-3.0.20260904150204}"
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_DIR="$SCRIPT_DIR/release"
COMMON_CFLAGS="-Wall -Wextra -g -O2 -std=c11 -Wno-format-truncation -DVUS_VERSION_STR=\"${VERSION}\""

echo "==== VUS 预编译包构建 v${VERSION} ===="
echo ""

# 清理
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

# 探测某 CC 能否编译 GUI 运行时（要求 libpng 头 + freetype 头 + C++ 编译器；
# 缺则跳过 GUI，与 install.sh 源码编译的 png.h 检测策略一致 —— 避免在无图形库系统上构建失败）
# 注意：仅本机构建（gcc/cc）启用 GUI；交叉编译器一律跳过——
#   1) 目标系统未必有 libpng/freetype/X11 开发包；
#   2) 用宿主 x86 头文件预检/编译交叉目标会带入位宽不兼容（如 32 位 time_t 差异），如 arm32 GuiLite 编译失败。
gui_available() {
    local cc="$1"
    local cxx_bin="$2"
    [ -n "$cxx_bin" ] || return 1
    case "$cc" in
        gcc|cc) ;;
        *) return 1 ;;
    esac
    printf '#include <png.h>\n' | "$cc" -E - >/dev/null 2>&1 || return 1
    printf '#include <ft2build.h>\n' | "$cc" $(pkg-config --cflags freetype2 2>/dev/null) -E - >/dev/null 2>&1
    return $?
}

# 构建单目标
build_target() {
    local target="$1"
    local cc="$2"
    local cxx="${3:-}"
    local build_dir="$SCRIPT_DIR/build-${target}"

    echo "构建 ${target} ..."
    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    # -------- Python 进程内嵌入检测（与 Makefile 一致）--------
    local py_def=""
    local py_inc=""
    if command -v python3-config >/dev/null 2>&1; then
        py_def="-DVUS_USE_PY"
        py_inc="$(python3-config --includes 2>/dev/null)"
    fi

    # -------- GUI 可用性（libpng + freetype + C++）--------
    local gui_def=""
    if [ -n "$cxx" ] && gui_available "$cc" "$cxx"; then
        gui_def="1"
        echo "  (检测到 libpng/freetype，构建含 GUI 图形运行时)"
    else
        echo "  (未检测到完整 GUI 依赖，仅构建纯运行时，跳过 GuiLite)"
    fi

    # -------- 运行时库 libvus_rt.a（与 Makefile 同构）--------
    echo "  编译运行时库 ..."
    local FREETYPE_INC="$(pkg-config --cflags freetype2 2>/dev/null || true)"

    $cc $COMMON_CFLAGS $py_def $py_inc -I"$SCRIPT_DIR/rt" -I"$SCRIPT_DIR/rt/easylogger/inc" \
        -c -o "$build_dir/libvus_rt.o" "$SCRIPT_DIR/rt/libvus_rt.c"

    $cc $COMMON_CFLAGS -I"$SCRIPT_DIR/rt" \
        -c -o "$build_dir/vus_coro.o" "$SCRIPT_DIR/rt/vus_coro.c"

    $cc -Wall -Wextra -O0 -std=c11 -I"$SCRIPT_DIR/rt" \
        -c -o "$build_dir/yyjson.o" "$SCRIPT_DIR/rt/yyjson/yyjson.c"

    $cc $COMMON_CFLAGS -I"$SCRIPT_DIR/rt/easylogger/inc" \
        -c -o "$build_dir/elog.o" "$SCRIPT_DIR/rt/easylogger/src/elog.c"
    $cc $COMMON_CFLAGS -I"$SCRIPT_DIR/rt/easylogger/inc" \
        -c -o "$build_dir/elog_utils.o" "$SCRIPT_DIR/rt/easylogger/src/elog_utils.c"
    $cc $COMMON_CFLAGS -I"$SCRIPT_DIR/rt/easylogger/inc" -I"$SCRIPT_DIR/rt" \
        -c -o "$build_dir/elog_port.o" "$SCRIPT_DIR/rt/elog_port.c"

    $cc $COMMON_CFLAGS -I"$SCRIPT_DIR/rt" \
        -c -o "$build_dir/vus_xyz.o" "$SCRIPT_DIR/rt/vus_xyz.c"

    if [ -n "$gui_def" ]; then
        $cc $COMMON_CFLAGS $py_def $py_inc -I"$SCRIPT_DIR/rt" -DVUS_GUI_X11 $FREETYPE_INC \
            -c -o "$build_dir/guilite_bridge.o" "$SCRIPT_DIR/rt/guilite_bridge.c"
        $cc $COMMON_CFLAGS $py_def $py_inc -I"$SCRIPT_DIR/rt" -DVUS_GUI_X11 $FREETYPE_INC \
            -c -o "$build_dir/guilite_platform.o" "$SCRIPT_DIR/rt/guilite_platform.c"
        $cxx -Wall -Wextra -g -O2 -I"$SCRIPT_DIR/rt" -I"$SCRIPT_DIR/rt/guilite" \
            -c -o "$build_dir/guilite_wrapper.o" "$SCRIPT_DIR/rt/guilite_wrapper.cpp"
        $cc $COMMON_CFLAGS -I"$SCRIPT_DIR/rt" \
            -c -o "$build_dir/gifdec.o" "$SCRIPT_DIR/rt/gifdec/gifdec.c"
    fi

    ar rcs "$build_dir/libvus_rt.a" "$build_dir/"libvus_rt.o "$build_dir/"vus_coro.o \
        "$build_dir/"yyjson.o "$build_dir/"elog.o "$build_dir/"elog_utils.o \
        "$build_dir/"elog_port.o "$build_dir/"vus_xyz.o \
        $( [ -n "$gui_def" ] && echo "$build_dir/"guilite_bridge.o "$build_dir/"guilite_platform.o "$build_dir/"guilite_wrapper.o "$build_dir/"gifdec.o )

    # -------- 编译器（全部子命令，含 chart/vaz/lsp）--------
    echo "  编译编译器 ..."
    local srcs="main token lexer parser generator config ast vus_abi vus_plugin vus_lang vus_vusx vus_apk vus_chart vus_vaz"
    for src in $srcs; do
        $cc $COMMON_CFLAGS -I"$SCRIPT_DIR/src" -I"$SCRIPT_DIR/rt/yyjson" -c -o "$build_dir/${src}.o" "$SCRIPT_DIR/src/${src}.c"
    done
    $cc $COMMON_CFLAGS -I"$SCRIPT_DIR/src" -c -o "$build_dir/vus_builtin.o" "$SCRIPT_DIR/src/lsp/vus_builtin.c"
    $cc $COMMON_CFLAGS -I"$SCRIPT_DIR/src" -I"$SCRIPT_DIR/rt" -c -o "$build_dir/lsp.o" "$SCRIPT_DIR/src/lsp/lsp.c"

    $cc -o "$build_dir/vus" "$build_dir/"main.o "$build_dir/"token.o "$build_dir/"lexer.o \
        "$build_dir/"parser.o "$build_dir/"generator.o "$build_dir/"config.o "$build_dir/"ast.o \
        "$build_dir/"vus_abi.o "$build_dir/"vus_plugin.o "$build_dir/"vus_lang.o \
        "$build_dir/"vus_vusx.o "$build_dir/"vus_apk.o "$build_dir/"vus_chart.o \
        "$build_dir/"vus_vaz.o "$build_dir/"lsp.o "$build_dir/"vus_builtin.o \
        "$build_dir/"yyjson.o -lm -ldl
    chmod +x "$build_dir/vus"

    # -------- 打包（vus-<arch>/ 目录形态，与 install.sh 一致）--------
    local pkg_name="vus-${target}"
    local pkg_dir="$RELEASE_DIR/${pkg_name}"
    mkdir -p "$pkg_dir"

    cp "$build_dir/vus" "$pkg_dir/"
    # 生成器固定从 <安装目录>/build/libvus_rt.a 链接静态运行时
    mkdir -p "$pkg_dir/build"
    cp "$build_dir/libvus_rt.a" "$pkg_dir/build/"

    # 随包文件：运行时源码/头（生成代码编译期使用 rt 目录）
    cp -r "$SCRIPT_DIR/rt" "$pkg_dir/rt"
    # 脚本（vux 插件管理/入口）
    mkdir -p "$pkg_dir/scripts"
    cp "$SCRIPT_DIR/scripts/vux_plugin_manager.py" "$SCRIPT_DIR/scripts/vux_plugin_entry.py" "$pkg_dir/scripts/"
    # 公共 API 头文件
    mkdir -p "$pkg_dir/include/vus"
    cp "$SCRIPT_DIR/include/vus/"*.h "$pkg_dir/include/vus/"
    # 示例（install.sh 一键测试用到 examples/hello.vus；其余按需随包）
    mkdir -p "$pkg_dir/examples"
    cp "$SCRIPT_DIR/examples/hello.vus" "$pkg_dir/examples/"
    # 测试用例（安装后 vus test / 一键测试需要）
    mkdir -p "$pkg_dir/tests"
    cp "$SCRIPT_DIR/tests/test_hello.vus" "$SCRIPT_DIR/tests/test_variables.vus" \
       "$SCRIPT_DIR/tests/test_arithmetic.vus" "$SCRIPT_DIR/tests/test_control.vus" \
       "$SCRIPT_DIR/tests/test_functions.vus" "$SCRIPT_DIR/tests/test_comparison.vus" \
       "$SCRIPT_DIR/tests/test_string.vus" "$SCRIPT_DIR/tests/test_while_count.vus" \
       "$SCRIPT_DIR/tests/test_factorial.vus" "$SCRIPT_DIR/tests/test_fibonacci.vus" \
       "$SCRIPT_DIR/tests/test_nested_control.vus" "$SCRIPT_DIR/tests/test_comprehensive.vus" \
       "$SCRIPT_DIR/tests/test_demo.vus" "$pkg_dir/tests/" 2>/dev/null || true
    chmod +x "$pkg_dir/vus"

    # -------- 版本信息 --------
    cat > "$pkg_dir/version.json" <<VERSION_EOF
{
    "名称": "VUS 编译器",
    "版本": "${VERSION}",
    "架构": "${target}",
    "构建日期": "$(date '+%Y-%m-%d %H:%M:%S')"
}
VERSION_EOF

    # -------- 包内安装脚本（适用同架构/兼容系统）--------
    cat > "$pkg_dir/install.sh" << 'INSTALL_EOF'
#!/bin/sh
# VUS 预编译包安装脚本（包内一键安装）
INSTALL_DIR="${VUS_HOME:-$HOME/.vus}"
echo "安装 VUS 到 $INSTALL_DIR ..."
mkdir -p "$INSTALL_DIR"
cp -r "$(dirname "$0")/vus" "$(dirname "$0")/rt" "$(dirname "$0")/scripts" \
      "$(dirname "$0")/include" "$(dirname "$0")/tests" "$(dirname "$0")/examples" \
      "$(dirname "$0")/build" "$(dirname "$0")/version.json" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/vus"
mkdir -p "$HOME/.local/bin"
ln -sf "$INSTALL_DIR/vus" "$HOME/.local/bin/vus"
echo "安装完成！"
echo "请确保 ~/.local/bin 在 PATH 中，运行: vus --version"
INSTALL_EOF
    chmod +x "$pkg_dir/install.sh"

    # -------- tarball + 校验和 --------
    cd "$RELEASE_DIR"
    tar czf "${pkg_name}.tar.gz" "${pkg_name}"
    rm -rf "${pkg_name}"

    sha256sum "${pkg_name}.tar.gz" > "${pkg_name}.tar.gz.sha256"
    md5sum "${pkg_name}.tar.gz" | awk '{print $1}' > "${pkg_name}.tar.gz.md5"

    echo "  ✅ ${pkg_name}.tar.gz"
    echo ""
}

# 构建各平台
echo "==== 构建 amd64 (x86_64) ===="
build_target "amd64" "gcc" "g++"

echo "==== 构建 arm64 (aarch64) ===="
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    build_target "arm64" "aarch64-linux-gnu-gcc" "aarch64-linux-gnu-g++"
else
    echo "  ⚠️  未找到 aarch64-linux-gnu-gcc，跳过 ARM64 构建"
    echo "  安装: sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu"
fi

echo "==== 构建 arm32 (armhf) ===="
if command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
    build_target "arm32" "arm-linux-gnueabihf-gcc" "arm-linux-gnueabihf-g++"
else
    echo "  ⚠️  未找到 arm-linux-gnueabihf-gcc，跳过 ARM32 构建"
    echo "  安装: sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf"
fi

echo "==== 构建 x8632 (i686) ===="
if command -v i686-linux-gnu-gcc >/dev/null 2>&1; then
    build_target "x8632" "i686-linux-gnu-gcc" "i686-linux-gnu-g++"
else
    echo "  ⚠️  未找到 i686-linux-gnu-gcc，跳过 X86-32 构建"
    echo "  安装: sudo apt install gcc-i686-linux-gnu g++-i686-linux-gnu"
fi

# 生成总校验和
cd "$RELEASE_DIR"
echo "==== MD5 校验和 ===="
cat *.md5 2>/dev/null || echo "(无 MD5 校验和文件)"
echo ""
echo "==== SHA256 校验和 ===="
cat *.sha256 2>/dev/null || echo "(无 SHA256 校验和文件)"
echo ""
echo "==== 构建完成 ===="
echo "发布目录: $RELEASE_DIR"
ls -lh "$RELEASE_DIR/"*.tar.gz 2>/dev/null || echo "(无二进制包)"