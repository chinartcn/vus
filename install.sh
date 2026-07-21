#!/usr/bin/env bash
# VUS 一键安装脚本
# 用法: curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | bash

set -e

REPO_URL="https://gitee.com/rtccn_mc/vus.git"
INSTALL_DIR="${VUS_HOME:-$HOME/.vus}"

echo "==== VUS 语言安装工具 v0.1 ===="
echo ""

# 检查依赖
echo "检查依赖..."

# 检查 git
if ! command -v git &>/dev/null; then
    echo "错误: 未找到 git，请先安装 Git"
    echo "  Ubuntu/Debian: sudo apt install git"
    echo "  CentOS/RHEL:   sudo yum install git"
    echo "  Termux:        pkg install git"
    exit 1
fi
echo "  ✅ git"

# 检查 Python
if ! command -v python3 &>/dev/null; then
    echo "错误: 未找到 python3，请先安装 Python 3"
    echo "  Ubuntu/Debian: sudo apt install python3"
    echo "  Termux:        pkg install python"
    exit 1
fi
echo "  ✅ python3"

# 检查 GCC
if ! command -v gcc &>/dev/null; then
    echo "  ⚠️  未找到 gcc，编译 --exe 模式需要 GCC"
    echo "  Ubuntu/Debian: sudo apt install gcc"
    echo "  Termux:        pkg install gcc"
    echo "  如果只需要 vus build --c-only，可以跳过"
fi
echo ""

# 克隆仓库
echo "正在安装 VUS..."
if [ -d "$INSTALL_DIR" ]; then
    echo "检测到已有安装，正在更新..."
    cd "$INSTALL_DIR"
    git pull --ff-only
else
    git clone --depth 1 "$REPO_URL" "$INSTALL_DIR"
fi

# 设置可执行权限
chmod +x "$INSTALL_DIR/vus"

# 创建符号链接
echo ""
echo "设置 PATH..."
if [ -d "$HOME/.local/bin" ]; then
    ln -sf "$INSTALL_DIR/vus" "$HOME/.local/bin/vus"
    echo "  ✅ 已创建符号链接: $HOME/.local/bin/vus"
    echo "  请确保 $HOME/.local/bin 在 PATH 中"
elif [ -d "$HOME/bin" ]; then
    ln -sf "$INSTALL_DIR/vus" "$HOME/bin/vus"
    echo "  ✅ 已创建符号链接: $HOME/bin/vus"
else
    mkdir -p "$HOME/.local/bin"
    ln -sf "$INSTALL_DIR/vus" "$HOME/.local/bin/vus"
    echo "  ✅ 已创建符号链接: $HOME/.local/bin/vus"
    echo "  请将以下内容添加到 ~/.bashrc 或 ~/.zshrc："
    echo "    export PATH=\"\$HOME/.local/bin:\$PATH\""
fi

# 验证安装
echo ""
echo "验证安装..."
if "$INSTALL_DIR/vus" --help &>/dev/null; then
    echo "  ✅ VUS 安装成功！"
    echo ""
    echo "快速开始："
    echo "  vus init               # 初始化项目"
    echo "  vus run main.vus       # 编译并运行"
    echo "  vus build --c-only     # 仅编译为 C 代码"
    echo "  vus build --exe        # 编译为可执行文件"
else
    echo "  ❌ 安装验证失败"
    exit 1
fi