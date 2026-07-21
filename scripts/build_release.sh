#!/bin/bash
# VUS 预编译包构建脚本
# 用法: bash scripts/build_release.sh [版本号]
# 依赖: gcc, aarch64-linux-gnu-gcc, arm-linux-gnueabihf-gcc

set -e

VERSION="${1:-$(date +%Y%m%d)}"
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_DIR="$SCRIPT_DIR/release"

echo "==== VUS 预编译包构建 v${VERSION} ===="
echo ""

# 清理
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

# 定义构建函数
build_target() {
    local target="$1"
    local cc="$2"
    local suffix="$3"
    local build_dir="$SCRIPT_DIR/build-${target}"
    
    echo "构建 ${target}..."
    rm -rf "$build_dir"
    mkdir -p "$build_dir"
    
    # 编译运行时库
    $cc -Wall -Wextra -g -O2 -std=c11 -I"$SCRIPT_DIR/rt" -c -o "$build_dir/libvus_rt.o" "$SCRIPT_DIR/rt/libvus_rt.c"
    ar rcs "$build_dir/libvus_rt.a" "$build_dir/libvus_rt.o"
    
    # 编译编译器
    for src in main token lexer parser generator config ast vus_abi vus_plugin vus_lang vus_vusx; do
        $cc -Wall -Wextra -g -O2 -std=c11 -Wno-format-truncation -I"$SCRIPT_DIR/src" -c -o "$build_dir/${src}.o" "$SCRIPT_DIR/src/${src}.c"
    done
    
    $cc -o "$build_dir/vus" "$build_dir/"*.o -lm -ldl
    chmod +x "$build_dir/vus"
    
    # 打包
    local pkg_name="vus-${VERSION}-${target}"
    local pkg_dir="$RELEASE_DIR/${pkg_name}"
    mkdir -p "$pkg_dir"
    
    cp "$build_dir/vus" "$pkg_dir/"
    cp -r "$SCRIPT_DIR/rt" "$pkg_dir/rt"
    mkdir -p "$pkg_dir/构建"
    
    # 创建安装脚本
    cat > "$pkg_dir/install.sh" << 'INSTALL_EOF'
#!/bin/sh
# VUS 预编译包安装脚本
INSTALL_DIR="${VUS_HOME:-$HOME/.vus}"
echo "安装 VUS 到 $INSTALL_DIR ..."
mkdir -p "$INSTALL_DIR"
cp -r "$(dirname "$0")/vus" "$INSTALL_DIR/"
cp -r "$(dirname "$0")/rt" "$INSTALL_DIR/"
mkdir -p "$HOME/.local/bin"
ln -sf "$INSTALL_DIR/vus" "$HOME/.local/bin/vus"
echo "安装完成！"
echo "请确保 ~/.local/bin 在 PATH 中"
echo "运行: vus --help"
INSTALL_EOF
    chmod +x "$pkg_dir/install.sh"
    
    # 创建 tarball
    cd "$RELEASE_DIR"
    tar czf "${pkg_name}.tar.gz" "${pkg_name}"
    rm -rf "${pkg_name}"
    
    # 生成校验和
    sha256sum "${pkg_name}.tar.gz" > "${pkg_name}.tar.gz.sha256"
    
    echo "  ✅ ${pkg_name}.tar.gz"
    echo ""
}

# 构建各平台
echo "==== 构建 amd64 (x86_64) ===="
build_target "amd64" "gcc" ""

echo "==== 构建 arm64 (aarch64) ===="
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    build_target "arm64" "aarch64-linux-gnu-gcc" ""
else
    echo "  ⚠️  未找到 aarch64-linux-gnu-gcc，跳过 ARM64 构建"
    echo "  安装: sudo apt install gcc-aarch64-linux-gnu"
fi

echo "==== 构建 arm32 (armhf) ===="
if command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
    build_target "arm32" "arm-linux-gnueabihf-gcc" ""
else
    echo "  ⚠️  未找到 arm-linux-gnueabihf-gcc，跳过 ARM32 构建"
    echo "  安装: sudo apt install gcc-arm-linux-gnueabihf"
fi

# 生成总校验和
cd "$RELEASE_DIR"
echo "==== 校验和 ===="
cat *.sha256 2>/dev/null || echo "(无校验和文件)"
echo ""
echo "==== 构建完成 ===="
echo "发布目录: $RELEASE_DIR"
ls -lh "$RELEASE_DIR/"*.tar.gz 2>/dev/null || echo "(无二进制包)"