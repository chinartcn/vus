#!/bin/sh
# VUS 一键安装脚本
# 用法: curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | sh

set -e

REPO_URL="https://gitee.com/rtccn_mc/vus.git"
INSTALL_DIR="${VUS_HOME:-$HOME/.vus}"

echo "==== VUS 语言安装工具 v0.1 ===="
echo ""

# 检查依赖
echo "检查依赖..."

# 检查 git
if ! command -v git >/dev/null 2>&1; then
    echo "错误: 未找到 git，请先安装 Git"
    echo "  Ubuntu/Debian: sudo apt install git"
    echo "  CentOS/RHEL:   sudo yum install git"
    echo "  Termux:        pkg install git"
    exit 1
fi
echo "  ✅ git"

# 检查 GCC
if ! command -v gcc >/dev/null 2>&1; then
    echo "错误: 未找到 gcc，请先安装 GCC"
    echo "  Ubuntu/Debian: sudo apt install gcc"
    echo "  CentOS/RHEL:   sudo yum install gcc"
    echo "  Termux:        pkg install gcc"
    exit 1
fi
echo "  ✅ gcc"

# 检查 make
if ! command -v make >/dev/null 2>&1; then
    echo "错误: 未找到 make，请先安装 make"
    echo "  Ubuntu/Debian: sudo apt install make"
    echo "  CentOS/RHEL:   sudo yum install make"
    exit 1
fi
echo "  ✅ make"
echo ""

# 克隆仓库
echo "正在安装 VUS..."
if [ -d "$INSTALL_DIR" ]; then
    echo "检测到已有安装，正在更新..."
    cd "$INSTALL_DIR"
    git pull --ff-only
else
    git clone --depth 1 "$REPO_URL" "$INSTALL_DIR"
    cd "$INSTALL_DIR"
fi

# 编译 VUS 编译器
echo ""
echo "编译 VUS 编译器..."
if make -C "$INSTALL_DIR" 2>&1; then
    echo "  ✅ 编译成功"
else
    echo "  ❌ 编译失败"
    exit 1
fi

# 设置可执行权限
chmod +x "$INSTALL_DIR/vus"

# 创建符号链接
echo ""
echo "设置 PATH..."
TARGET_DIR="$HOME/.local/bin"
if [ ! -d "$HOME/.local/bin" ]; then
    mkdir -p "$HOME/.local/bin"
fi
ln -sf "$INSTALL_DIR/vus" "$TARGET_DIR/vus"
echo "  ✅ 已创建符号链接: $TARGET_DIR/vus"

# 自动将 TARGET_DIR 加入 PATH（如果尚未在 PATH 中）
case ":$PATH:" in
    *":$TARGET_DIR:"*) ;;
    *)
        # 检测当前 shell 并写入对应的配置文件
        LINE="export PATH=\"\$HOME/.local/bin:\$PATH\""
        RC_FILE=""
        SHELL_NAME="$(basename "${SHELL:-/bin/sh}")"
        case "$SHELL_NAME" in
            bash)
                RC_FILE="$HOME/.bashrc"
                REFRESH_CMD="source $RC_FILE"
                ;;
            zsh)
                RC_FILE="$HOME/.zshrc"
                REFRESH_CMD="source $RC_FILE"
                ;;
            fish)
                RC_FILE="$HOME/.config/fish/config.fish"
                mkdir -p "$(dirname "$RC_FILE")"
                LINE="fish_add_path $TARGET_DIR"
                REFRESH_CMD="source $RC_FILE"
                ;;
            *)  # ash, sh, dash 等 POSIX shell
                RC_FILE="$HOME/.profile"
                REFRESH_CMD=". $RC_FILE"
                ;;
        esac
        if [ -n "$RC_FILE" ]; then
            mkdir -p "$(dirname "$RC_FILE")"
            grep -qxF "$LINE" "$RC_FILE" 2>/dev/null || echo "$LINE" >> "$RC_FILE"
            echo "  ✅ 已写入 $RC_FILE"
        fi
        # 也写入 .profile 作为后备（兼容所有 POSIX shell）
        if [ "$RC_FILE" != "$HOME/.profile" ]; then
            grep -qxF "export PATH=\"\$HOME/.local/bin:\$PATH\"" "$HOME/.profile" 2>/dev/null || \
                echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$HOME/.profile"
        fi
        echo "  ⚠️  请执行以下命令刷新 PATH，或重新打开终端："
        echo "     $REFRESH_CMD"
        ;;
esac

# 验证安装
echo ""
echo "验证安装..."
if "$INSTALL_DIR/vus" --help >/dev/null 2>&1; then
    echo "  ✅ VUS 安装成功！"
    echo ""
    echo "快速开始（执行上方刷新命令后）："
    echo "  vus init               # 初始化项目"
    echo "  vus run main.vus       # 编译并运行"
    echo "  vus build --c-only     # 仅编译为 C 代码"
    echo "  vus build --exe        # 编译为可执行文件"
    echo ""
    echo "当前终端可立即使用（通过完整路径）："
    echo "  $INSTALL_DIR/vus init"
else
    echo "  ❌ 安装验证失败"
    exit 1
fi