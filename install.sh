#!/bin/sh
# VUS 一键安装脚本
# 用法: curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | sh
#        curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | VUS_HOME=/opt/vus sh

set -e

REPO_URL="https://gitee.com/rtccn_mc/vus.git"
RELEASE_BASE="https://gitee.com/rtccn_mc/vus/releases/download"
INSTALL_DIR="${VUS_HOME:-$HOME/.vus}"
VERSION="latest"

echo ""
echo -ne "\033[48;5;25m      \033[0m                                                                  \033[0m"
echo -ne " \033[48;5;25m    \033[48;5;26m    \033[0m                                                               \033[0m"
echo -ne " \033[48;5;25m     \033[48;5;26m    \033[0m        \033[48;5;31m    \033[0m   \033[48;5;31m \033[48;5;32m   \033[0m        \033[48;5;37m    \033[48;5;38m \033[0m \033[48;5;37m \033[48;5;38m   \033[0m       \033[48;5;43m   \033[48;5;44m \033[0m   \033[48;5;43m  \033[48;5;44m       \033[0m  \033[0m"
echo -ne "  \033[48;5;25m   \033[48;5;26m \033[0m \033[48;5;26m    \033[48;5;31m \033[0m      \033[48;5;31m    \033[0m    \033[48;5;32m    \033[0m      \033[48;5;37m     \033[0m  \033[48;5;38m    \033[0m      \033[48;5;38m \033[48;5;43m    \033[0m  \033[48;5;43m \033[48;5;44m         \033[0m  \033[0m"
echo -ne "  \033[48;5;25m   \033[48;5;26m \033[0m  \033[48;5;26m    \033[0m     \033[48;5;31m    \033[48;5;37m \033[0m    \033[48;5;32m     \033[0m     \033[48;5;37m    \033[0m   \033[48;5;37m \033[48;5;38m   \033[0m      \033[48;5;38m \033[48;5;43m   \033[48;5;44m \033[0m \033[48;5;43m   \033[48;5;44m  \033[0m        \033[0m"
echo -ne "  \033[48;5;25m    \033[48;5;31m \033[0m \033[48;5;26m    \033[48;5;31m \033[0m    \033[48;5;31m    \033[0m      \033[48;5;32m    \033[48;5;37m \033[0m   \033[48;5;37m    \033[0m    \033[48;5;37m \033[48;5;38m   \033[0m      \033[48;5;38m \033[48;5;43m    \033[0m  \033[48;5;43m \033[48;5;44m       \033[0m    \033[0m"
echo -ne "   \033[48;5;25m   \033[48;5;26m \033[0m  \033[48;5;26m    \033[0m   \033[48;5;26m \033[48;5;31m   \033[0m        \033[48;5;32m    \033[0m  \033[48;5;32m \033[48;5;37m   \033[0m     \033[48;5;37m \033[48;5;38m   \033[0m      \033[48;5;38m \033[48;5;43m    \033[0m    \033[48;5;43m \033[48;5;44m        \033[0m \033[0m"
echo -ne "   \033[48;5;25m   \033[48;5;26m \033[0m  \033[48;5;26m    \033[48;5;32m \033[0m  \033[48;5;31m    \033[0m         \033[48;5;32m    \033[48;5;37m     \033[0m     \033[48;5;37m \033[48;5;38m    \033[0m     \033[48;5;43m    \033[0m          \033[48;5;44m    \033[48;5;49m \033[0m"
echo -ne "    \033[48;5;25m   \033[48;5;26m \033[0m  \033[48;5;26m   \033[0m  \033[48;5;26m \033[48;5;31m   \033[0m           \033[48;5;32m   \033[48;5;37m    \033[0m       \033[48;5;38m         \033[48;5;43m    \033[0m   \033[48;5;43m    \033[0m  \033[48;5;44m     \033[0m \033[0m"
echo -ne "    \033[48;5;25m   \033[48;5;26m \033[0m  \033[48;5;26m  \033[48;5;31m \033[0m  \033[48;5;26m \033[48;5;31m   \033[0m            \033[48;5;32m   \033[48;5;37m  \033[0m         \033[48;5;38m       \033[48;5;43m   \033[0m    \033[48;5;43m   \033[48;5;44m        \033[0m  \033[0m"
echo -ne "     \033[48;5;26m    \033[0m     \033[48;5;26m \033[48;5;31m   \033[0m                                                      \033[0m"
echo -ne "      \033[48;5;25m \033[48;5;26m    \033[0m   \033[48;5;26m \033[48;5;31m   \033[0m                                                      \033[0m"
echo -ne "       \033[48;5;25m \033[48;5;26m       \033[48;5;31m  \033[0m                                                       \033[0m"
echo -ne "          \033[48;5;26m      \033[48;5;31m \033[0m                                                       \033[0m"
echo ""
echo -e "\033[36m  VUS 编程语言 — 安装工具 v3.0.20260904150204（正式版）\033[0m"
echo -e "\033[36m  中西合璧，天下无敌\033[0m"
echo -e "\033[36m============================================\033[0m"
echo ""
progress() {
    local msg="$1"
    printf "  %s" "$msg"
    local i=0
    while [ $i -lt 10 ]; do
        printf "."
        i=$((i + 1))
        sleep 0.1
    done
    printf "\n"
}

# 探测 GUI 图形运行时能否编译（libpng + FreeType 头）：
# 用 gcc 预处理器 + pkg-config 探测，覆盖非默认头目录（如 /usr/include/freetype2）；
# 与 Makefile / build_release.sh 的判定保持一致。gcc 不可用时视为缺失（会走核心依赖安装）。
has_gui_headers() {
    if ! command -v gcc >/dev/null 2>&1; then
        return 1
    fi
    if ! printf '#include <png.h>\n' | gcc -x c -E - >/dev/null 2>&1; then
        return 1
    fi
    FREETYPE_CFLAGS="$(pkg-config --cflags freetype2 2>/dev/null || true)"
    printf '#include <ft2build.h>\n' | gcc -x c -E $FREETYPE_CFLAGS - >/dev/null 2>&1
}

echo ""

# 检测系统架构
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64)     PKG_ARCH="amd64" ;;
    aarch64|arm64)    PKG_ARCH="arm64" ;;
    armv7l|armv6l|arm) PKG_ARCH="arm32" ;;
    *)                PKG_ARCH="" ;;
esac

# 检查下载工具
DOWNLOAD_CMD=""
if command -v curl >/dev/null 2>&1; then
    DOWNLOAD_CMD="curl -fsSL"
elif command -v wget >/dev/null 2>&1; then
    DOWNLOAD_CMD="wget -qO-"
fi

# 检查 git（源码编译时需要）
GIT_AVAILABLE=0
command -v git >/dev/null 2>&1 && GIT_AVAILABLE=1

echo "系统架构: $ARCH"
echo ""

# ============================================================
# 尝试预编译包安装（仅支持的架构）
# ============================================================
INSTALL_FROM_SOURCE=0

if [ -n "$PKG_ARCH" ] && [ -n "$DOWNLOAD_CMD" ]; then
    PKG_NAME="vus-${PKG_ARCH}.tar.gz"
    PKG_URL="${RELEASE_BASE}/${VERSION}/${PKG_NAME}"

    echo "尝试下载预编译包: ${PKG_NAME}"

    TMP_DIR=$(mktemp -d 2>&1 || mktemp -d /tmp/vus_install.XXXXXX)
    progress "下载中..."
    if $DOWNLOAD_CMD "$PKG_URL" > "$TMP_DIR/$PKG_NAME" 2>&1 && [ -s "$TMP_DIR/$PKG_NAME" ]; then
        echo "  ✅ 预编译包下载成功"
        echo ""

        # MD5 校验
        PKG_MD5_URL="${PKG_URL}.md5"
        EXPECTED_MD5=""
        if $DOWNLOAD_CMD "$PKG_MD5_URL" > "$TMP_DIR/$PKG_NAME.md5" 2>&1 && [ -s "$TMP_DIR/$PKG_NAME.md5" ]; then
            EXPECTED_MD5=$(cat "$TMP_DIR/$PKG_NAME.md5" | tr -d ' \t\n\r')
            COMPUTED_MD5=$(md5sum "$TMP_DIR/$PKG_NAME" 2>&1 | awk '{print $1}')
            if [ "$COMPUTED_MD5" = "$EXPECTED_MD5" ]; then
                echo "  ✅ MD5 校验通过: $COMPUTED_MD5"
            else
                echo "  ❌ MD5 校验失败:"
                echo "     期望: $EXPECTED_MD5"
                echo "     实际: $COMPUTED_MD5"
                echo "     文件可能已损坏或被篡改，切换到源码编译"
                rm -rf "$TMP_DIR"
                INSTALL_FROM_SOURCE=1
            fi
        else
            echo "  ⚠️  未找到 MD5 校验文件，跳过校验"
        fi

        if [ "$INSTALL_FROM_SOURCE" -eq 0 ]; then
        echo "正在安装..."

        mkdir -p "$INSTALL_DIR"
        tar xzf "$TMP_DIR/$PKG_NAME" -C "$TMP_DIR"
        # 版本校验
        if [ -f "$TMP_DIR/vus-${PKG_ARCH}/version.json" ]; then
            PKG_VER=$(grep '"版本"' "$TMP_DIR/vus-${PKG_ARCH}/version.json" | cut -d'"' -f4)
            echo "  📦 版本: ${PKG_VER}"
        fi
        cp -r "$TMP_DIR/vus-${PKG_ARCH}/"* "$INSTALL_DIR/"
        chmod +x "$INSTALL_DIR/vus"
        rm -rf "$TMP_DIR"

        echo "  ✅ 预编译包安装完成"
        fi
    else
        rm -rf "$TMP_DIR"
        echo "  ⚠️  预编译包下载失败，切换到源码编译"
        INSTALL_FROM_SOURCE=1
    fi
else
    INSTALL_FROM_SOURCE=1
fi

# ============================================================
# 源码编译安装（无预编译包或下载失败时）
# ============================================================
if [ "$INSTALL_FROM_SOURCE" -eq 1 ]; then
    echo "使用源码编译安装..."

    # ---------- 自动检测系统并处理缺失依赖 ----------
    # 读取发行版信息（/etc/os-release），据此给出正确的包管理器与依赖组
    if [ -r /etc/os-release ]; then
        . /etc/os-release
    fi
    DETECT_UPDATE=""
    DETECT_INSTALL=""
    DETECT_GROUP=""
    DETECT_GUI_GROUP=""
    case "$ID" in
        alpine)                       # musl libc：build-base 提供 gcc/make/musl-dev(头文件) 与 g++
            DETECT_UPDATE="apk update"
            DETECT_INSTALL="apk add"
            DETECT_GROUP="build-base git"
            DETECT_GUI_GROUP="libpng-dev freetype-dev libx11-dev"
            ;;
        debian|ubuntu|linuxmint)
            DETECT_UPDATE="apt-get update"
            DETECT_INSTALL="apt-get install -y"
            DETECT_GROUP="build-essential git"
            DETECT_GUI_GROUP="libpng-dev libfreetype-dev libx11-dev g++"
            ;;
        centos|rhel|fedora|rocky|almalinux)
            DETECT_INSTALL="dnf install -y"
            DETECT_GROUP="gcc make glibc-devel git"
            DETECT_GUI_GROUP="libpng-devel freetype-devel libX11-devel gcc-c++"
            ;;
        arch|manjaro|endeavouros)
            DETECT_UPDATE="pacman -Sy"
            DETECT_INSTALL="pacman -S --noconfirm"
            DETECT_GROUP="base-devel git"
            DETECT_GUI_GROUP="libpng freetype2 libx11"
            ;;
        opensuse*|sles)
            DETECT_INSTALL="zypper install -y"
            DETECT_GROUP="gcc make glibc-devel git"
            DETECT_GUI_GROUP="libpng-devel freetype2-devel libX11-devel gcc-c++"
            ;;
    esac

    # 检测 GUI 图形依赖是否已具备（libpng + FreeType 开发头，gcc 探测）：
    # 已具备 → 编译阶段会自动 make all 构建 GUI 运行时；缺失 → 询问是否补装
    GUI_HEADERS_PRESENT=0
    if has_gui_headers; then
        GUI_HEADERS_PRESENT=1
    fi

    echo "检查依赖..."
    if [ -n "$DETECT_GROUP" ]; then
        echo "  检测到系统: ${PRETTY_NAME:-$ID}"

        # 逐个检测基础工具是否可用
        MISSING=""
        for d in git gcc make; do
            if ! command -v "$d" >/dev/null 2>&1; then
                MISSING="$MISSING $d"
            fi
        done

        # 检测 C 头文件（如 stdio.h）。只装了 gcc 没装 libc 头时，编译会报
        # "stdio.h: No such file or directory" —— 这正是缺失依赖的表现。
        if ! printf '#include <stdio.h>\n' | gcc -x c -E - >/dev/null 2>&1; then
            MISSING="$MISSING libc-dev(C 头文件)"
        fi

        if [ -z "$MISSING" ]; then
            echo "  ✅ 依赖齐全 (git / gcc / make / C 头文件)"
        else
            echo "  ⚠️  缺失依赖:${MISSING}"
            echo "  建议安装: $DETECT_INSTALL $DETECT_GROUP"

            # 决定是否自动安装：环境变量 VUS_AUTO_INSTALL=1，或交互输入 y
            DO_INSTALL="n"
            if [ "$VUS_AUTO_INSTALL" = "1" ]; then
                DO_INSTALL="y"
            elif [ -t 1 ]; then
                printf "  是否自动安装? (y 立即安装 / 其它 手动安装后退出) [y/N] "
                read -r ans
                case "$ans" in
                    y|Y) DO_INSTALL="y" ;;
                esac
            fi

            if [ "$DO_INSTALL" = "y" ]; then
                # 非 root 时加 sudo 前缀
                PRFX=""
                [ "$(id -u)" = "0" ] || PRFX="sudo "
                if [ -n "$DETECT_UPDATE" ]; then
                    ${PRFX}${DETECT_UPDATE} || true
                fi
                if ${PRFX}${DETECT_INSTALL} $DETECT_GROUP; then
                    echo "  ✅ 依赖安装完成，继续编译"
                else
                    echo "  ❌ 依赖安装失败，请手动执行: ${PRFX}${DETECT_INSTALL} $DETECT_GROUP"
                    exit 1
                fi
            else
                echo "  已跳过自动安装，请先手动执行: $DETECT_INSTALL $DETECT_GROUP"
                echo "  之后重跑本脚本即可；或设置 VUS_AUTO_INSTALL=1 让脚本自动安装。"
                exit 1
            fi
        fi
    else
        echo "  ⚠️  未能自动识别系统 ($ID)，请手动确认已安装: git / gcc / make / libc 头文件"
    fi

    # ---------- GUI 图形依赖（可选） ----------
    # 规则：
    #   VUS_GUI=1   → 无条件安装 GUI 依赖（自动，适合脚本化）
    #   VUS_GUI=0   → 跳过（默认）
    #   VUS_GUI 未设置且为交互终端 → 询问一次是否需要 GUI 图形支持
    if [ -n "$DETECT_GUI_GROUP" ] && [ "$GUI_HEADERS_PRESENT" -ne 1 ]; then
        WANT_GUI=""
        if [ "$VUS_GUI" = "1" ]; then
            WANT_GUI="1"
        elif [ "$VUS_GUI" != "0" ] && [ -t 1 ]; then
            printf "  GUI 图形支持（图形_* 画布流）需要 libpng/libfreetype/X11 开发包，是否一并安装? [y/N] "
            read -r gans
            case "$gans" in
                y|Y) WANT_GUI="1" ;;
            esac
        fi

        if [ "$WANT_GUI" = "1" ]; then
            PRFX=""
            [ "$(id -u)" = "0" ] || PRFX="sudo "
            echo "  安装 GUI 图形依赖: $DETECT_INSTALL $DETECT_GUI_GROUP"
            if [ -n "$DETECT_UPDATE" ]; then
                ${PRFX}${DETECT_UPDATE} || true
            fi
            if ${PRFX}${DETECT_INSTALL} $DETECT_GUI_GROUP; then
                echo "  ✅ GUI 图形依赖安装完成（将构建含 GUI 的完整运行时）"
            else
                echo "  ⚠️  GUI 依赖安装失败（不影响核心安装，可稍后手动安装后重跑本脚本）"
            fi
        elif [ "$VUS_GUI" = "1" ]; then
            : # 已尝试安装 VUS_GUI 明确要求
        else
            echo "  （跳过 GUI 图形依赖；如需 GUI 功能，可设置 VUS_GUI=1 重跑本脚本，"
            echo "    或在安装后安装 libpng-dev/libfreetype-dev/libx11-dev 再 make all）"
        fi
    fi
    echo ""

    # 克隆仓库
    echo "克隆源码..."
    if [ -d "$INSTALL_DIR/.git" ]; then
        echo "检测到已有安装，正在更新..."
        cd "$INSTALL_DIR"
        # 先确保远程地址正确
        git remote set-url origin "$REPO_URL" 2>/dev/null || git remote add origin "$REPO_URL"
        git fetch origin
        git reset --hard origin/master
    else
        # 目录存在但不是 git 仓库，删除后重新克隆
        [ -d "$INSTALL_DIR" ] && rm -rf "$INSTALL_DIR"
        git clone --depth 1 "$REPO_URL" "$INSTALL_DIR"
        cd "$INSTALL_DIR"
    fi

    # 编译 VUS 编译器
    echo ""
    echo "编译 VUS 编译器..."
    # 核心目标 `vus`（编译器 + LSP）只依赖 libc，任何系统都能编。
    # GUI 图形运行时(rt/guilite)额外依赖 libpng / FreeType / 桌面 X11，
    # 仅在检测到对应开发头时才尝试全量构建(包含 GUI)，否则跳过，
    # 避免在缺少图形库或不支持桌面的系统(如 ACode 的 Alpine)上编译失败。
    if has_gui_headers; then
        MAKE_TARGET="all"
        echo "  (检测到 libpng/libfreetype，构建含 GUI 图形运行时)"
    else
        MAKE_TARGET="vus"
        echo "  (未检测到 GUI 依赖，仅构建编译器+LSP，跳过 GUI 运行时)"
        echo "  提示: 如需 GUI 功能，请安装 libpng-dev libfreetype-dev libx11-dev 后重跑本脚本"
    fi
    progress "编译中..."
    if make -C "$INSTALL_DIR" "$MAKE_TARGET" 2>&1; then
        echo "  ✅ 编译成功"
    else
        echo "  ❌ 编译失败"
        echo "    若是在安装 GUI 依赖后仍失败，可尝试: make -C $INSTALL_DIR vus"
        exit 1
    fi

    chmod +x "$INSTALL_DIR/vus"
fi

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
REFRESH_CMD=""
case ":$PATH:" in
    *":$TARGET_DIR:"*)
        echo "  ✅ $TARGET_DIR 已在 PATH 中"
        ;;
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
            grep -qxF "$LINE" "$RC_FILE" 2>&1 || echo "$LINE" >> "$RC_FILE"
            echo "  ✅ 已写入 $RC_FILE"
        fi
        # 也写入 .profile 作为后备（兼容所有 POSIX shell）
        if [ "$RC_FILE" != "$HOME/.profile" ]; then
            grep -qxF "export PATH=\"\$HOME/.local/bin:\$PATH\"" "$HOME/.profile" 2>&1 || \
                echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$HOME/.profile"
        fi
        ;;
esac

# 验证安装
echo ""
echo "验证安装..."
if "$INSTALL_DIR/vus" --help >/dev/null 2>&1; then
    echo "  ✅ VUS 安装成功！"
    echo ""
    if [ -n "$REFRESH_CMD" ]; then
        echo "  ⚠️  请执行以下命令刷新 PATH，或重新打开终端："
        echo "     $REFRESH_CMD"
        echo ""
        echo "快速开始（执行上方刷新命令后）："
    else
        echo "快速开始："
    fi
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

# ============================================================
# 可选：Android SDK 环境（用于编译 APK / VUA 组件流）
# 询问是否安装 → 多来源探测宿主架构（兼容多种 uname/arch/dpkg 输出格式）
# → 按架构从 gitee release 下载组件 → 合并分卷 → 解压到 $HOME/.vus/android/
# ============================================================
ASDK_BASE="https://gitee.com/rtccn_mc/android-sdk/releases/download"
ANDK_BASE="https://gitee.com/rtccn_mc/android-ndk/releases/download"

# 多来源架构探测，归一化为: x86_64 / aarch64 / arm32 / unknown
# 覆盖：dpkg(权威)、uname -m、uname -p、arch、getconf LONG_BIT，
# 兼容 amd64/i386/arm64/armhf/armv7l/x86_64-linux-gnu 等多种输出。
detect_android_arch() {
    local dp ua
    # 1) dpkg --print-architecture（Debian/Ubuntu/Termux 皆有，最权威）
    if command -v dpkg >/dev/null 2>&1; then
        dp="$(dpkg --print-architecture 2>/dev/null)"
        case "$dp" in
            amd64) echo x86_64; return ;;
            arm64|aarch64) echo aarch64; return ;;
            armhf|armel|arm) echo arm32; return ;;
        esac
    fi
    # 2) 回退 uname -m / -p / arch（不同发行版输出格式不同）
    ua="$(uname -m 2>/dev/null)"
    [ -z "$ua" ] && ua="$(uname -p 2>/dev/null)"
    [ -z "$ua" ] && ua="$(arch 2>/dev/null)"
    case "$ua" in
        x86_64|amd64|x64|i86pc|ia64) echo x86_64; return ;;
        i386|i486|i586|i686|x86)
            # 32 位 x86 若实际为 64 位内核则归为 x86_64
            if command -v getconf >/dev/null 2>&1 && [ "$(getconf LONG_BIT 2>/dev/null)" = "64" ]; then
                echo x86_64; return
            fi
            echo unknown; return ;;
        aarch64|arm64|armv8l) echo aarch64; return ;;
        armv7l|armv7|armv6l|armhf|arm) echo arm32; return ;;
        *) echo unknown; return ;;
    esac
}

# 下载一个 URL 到文件（curl/wget 任一可用）
fetch_one() {
    local url="$1" dest="$2"
    mkdir -p "$(dirname "$dest")"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --connect-timeout 20 --retry 5 -o "$dest" "$url"
        rc=$?
    elif command -v wget >/dev/null 2>&1; then
        wget -q -T 30 -O "$dest" "$url"
        rc=$?
    else
        return 1
    fi
    [ $rc -eq 0 ] && [ -s "$dest" ] || return 1
}

# 合并分卷并解压到目标目录。
# base: 分卷前缀（如 xxx.tar.gz / xxx.tar.xz）；dest: 解压目录。
# 分卷形如 base.000/base.001…
cat_and_extract() {
    local base="$1" dest="$2" out v
    out="${base}.__merged__"
    rm -f "$out"
    for v in "${base}".0*; do
        [ -f "$v" ] || continue           # 跳过不存在的卷
        cat "$v" >> "$out"
    done
    [ -s "$out" ] || return 1
    mkdir -p "$dest"
    case "$base" in
        *.tar.xz) tar xJf "$out" -C "$dest" ;;
        *)        tar xzf "$out" -C "$dest" ;;
    esac
    rm -f "$out"
}

# 解压 zip（unzip 缺失时用 python3 兜底）
unzip_to() {
    local zip="$1" dest="$2"
    mkdir -p "$dest"
    if command -v unzip >/dev/null 2>&1; then
        unzip -oq "$zip" -d "$dest"
    elif command -v python3 >/dev/null 2>&1; then
        python3 - "$zip" "$dest" <<'PY'
import sys,zipfile
z=zipfile.ZipFile(sys.argv[1])
z.extractall(sys.argv[2])
z.close()
PY
    else
        return 1
    fi
}

install_android_sdk() {
    local AARCH ADROID TMPD ans RC=0
    # 询问是否安装（VUS_ANDROID=1 自动装，VUS_ANDROID=0 跳过，默认交互询问）
    if [ "$VUS_ANDROID" = "0" ]; then return 0; fi
    if [ "$VUS_ANDROID" != "1" ]; then
        if [ ! -t 1 ]; then return 0; fi        # 非交互终端默认跳过
        printf "  是否安装 Android SDK 环境（编译 APK / VUA 组件流用）? [y/N] "
        read -r ans
        case "$ans" in y|Y) ;; *) return 0 ;; esac
    fi

    AARCH="$(detect_android_arch)"
    echo "  ➤ 探测到宿主架构: $AARCH"

    ADROID="${VUS_ANDROID_DIR:-$HOME/.vus/android}"
    TMPD="$(mktemp -d 2>/dev/null || mktemp -d /tmp/vus_android.XXXXXX)"
    echo "  ➤ 安装目录: $ADROID"
    echo "  ➤ 开始下载 Android SDK 组件（约耗时数分钟，分卷自动合并）..."

    case "$AARCH" in
        x86_64)
            # ---- SDK 通用组件（All） ----
            fetch_one "$ASDK_BASE/sdk-v1/platforms.tar.gz" "$TMPD/platforms.tar.gz" \
                && cat_and_extract "$TMPD/platforms.tar.gz" "$ADROID/platforms" \
                || { echo "  ❌ platforms 下载失败"; RC=1; }
            fetch_one "$ASDK_BASE/sdk-v1/cmdline-tools.tar.gz.000" "$TMPD/c.000"
            fetch_one "$ASDK_BASE/sdk-v1/cmdline-tools.tar.gz.001" "$TMPD/c.001"
            { [ -s "$TMPD/c.000" ] && [ -s "$TMPD/c.001" ]; } && { mkdir -p "$ADROID/cmdline-tools"; cat "$TMPD/c.000" "$TMPD/c.001" > "$TMPD/clt.tar.gz" && tar xzf "$TMPD/clt.tar.gz" -C "$ADROID/cmdline-tools"; } || { echo "  ❌ cmdline-tools 下载失败"; RC=1; }
            # ---- build-tools + JDK（x86_64） ----
            fetch_one "$ASDK_BASE/sdk-v1/build-tools.tar.gz" "$TMPD/bt.tar.gz" \
                && cat_and_extract "$TMPD/bt.tar.gz" "$ADROID/build-tools" \
                || { echo "  ❌ build-tools 下载失败"; RC=1; }
            fetch_one "$ASDK_BASE/sdk-v1/jdk8.tar.gz.000" "$TMPD/j.000"
            fetch_one "$ASDK_BASE/sdk-v1/jdk8.tar.gz.001" "$TMPD/j.001"
            { [ -s "$TMPD/j.000" ] && [ -s "$TMPD/j.001" ]; } && { mkdir -p "$ADROID/jdk"; cat "$TMPD/j.000" "$TMPD/j.001" > "$TMPD/jdk.tar.gz" && tar xzf "$TMPD/jdk.tar.gz" -C "$ADROID/jdk"; } || { echo "  ❌ JDK8 下载失败"; RC=1; }
            # ---- NDK（v26.1，8 卷） ----
            for i in 0 1 2 3 4 5 6 7; do
                fetch_one "$ANDK_BASE/v26.1/ndk26.tar.gz.00$i" "$TMPD/n.00$i" || RC=1
            done
            if cat "$TMPD"/n.000 "$TMPD"/n.001 "$TMPD"/n.002 "$TMPD"/n.003 \
                    "$TMPD"/n.004 "$TMPD"/n.005 "$TMPD"/n.006 "$TMPD"/n.007 > "$TMPD/ndk.tar.gz" 2>/dev/null \
                && mkdir -p "$ADROID/ndk" && tar xzf "$TMPD/ndk.tar.gz" -C "$ADROID/ndk"; then
                : 
            else
                echo "  ❌ NDK26 下载/解压失败"; RC=1
            fi
            ;;
        aarch64)
            # 手机端/Termux arm64：Zulu JDK + 静态 build-tools + NDK29(aarch64,4卷/tar.xz)
            fetch_one "$ASDK_BASE/sdk-v2-arm/jdk8-aarch64.tar.gz.000" "$TMPD/j.000"
            fetch_one "$ASDK_BASE/sdk-v2-arm/jdk8-aarch64.tar.gz.001" "$TMPD/j.001"
            { [ -s "$TMPD/j.000" ] && [ -s "$TMPD/j.001" ]; } && { mkdir -p "$ADROID/jdk"; cat "$TMPD/j.000" "$TMPD/j.001" > "$TMPD/jdk.tar.gz" && tar xzf "$TMPD/jdk.tar.gz" -C "$ADROID/jdk"; } || { echo "  ❌ 手机端 JDK8 下载失败"; RC=1; }
            fetch_one "$ASDK_BASE/sdk-v2-arm/android-sdk-tools-static-aarch64.zip" "$TMPD/tools.zip" \
                && unzip_to "$TMPD/tools.zip" "$ADROID/build-tools" || { echo "  ❌ SDK tools 下载失败"; RC=1; }
            for i in 0 1 2 3; do
                fetch_one "$ANDK_BASE/v29-aarch64/ndk29-aarch64.tar.gz.00$i" "$TMPD/n.00$i" || RC=1
            done
            { cat "$TMPD"/n.000 "$TMPD"/n.001 "$TMPD"/n.002 "$TMPD"/n.003 > "$TMPD/ndk.tar.xz" 2>/dev/null \
                && mkdir -p "$ADROID/ndk" && tar xJf "$TMPD/ndk.tar.xz" -C "$ADROID/ndk"; } || { echo "  ❌ NDK29(aarch64) 下载失败"; RC=1; }
            ;;
        arm32)
            # 手机端/Termux arm32
            fetch_one "$ASDK_BASE/sdk-v2-arm/jdk8-arm32.tar.gz.000" "$TMPD/j.000"
            fetch_one "$ASDK_BASE/sdk-v2-arm/jdk8-arm32.tar.gz.001" "$TMPD/j.001"
            { [ -s "$TMPD/j.000" ] && [ -s "$TMPD/j.001" ]; } && { mkdir -p "$ADROID/jdk"; cat "$TMPD/j.000" "$TMPD/j.001" > "$TMPD/jdk.tar.gz" && tar xzf "$TMPD/jdk.tar.gz" -C "$ADROID/jdk"; } || { echo "  ❌ 手机端 JDK8 下载失败"; RC=1; }
            fetch_one "$ASDK_BASE/sdk-v2-arm/android-sdk-tools-static-arm.zip" "$TMPD/tools.zip" \
                && unzip_to "$TMPD/tools.zip" "$ADROID/build-tools" || { echo "  ❌ SDK tools 下载失败"; RC=1; }
            # NDK：Termux 用 deb 包安装（可在脚本外由用户自行 dpkg -i）
            echo "  ⚠️  arm32 的 NDK 由 Termux deb 包提供："
            echo "      下载 https://gitee.com/rtccn_mc/android-sdk/releases/download/sdk-v2-arm/ndk-multilib_29-1_all.deb"
            echo "        + ndk-sysroot_29-2_arm.deb 后执行: dpkg -i ndk-multilib_29-1_all.deb ndk-sysroot_29-2_arm.deb"
            ;;
        *)
            echo "  ⚠️  无法识别架构，跳过 Android SDK 安装。"
            ;;
    esac

    rm -rf "$TMPD"
    if [ "$RC" -eq 0 ]; then
        echo "  ✅ Android SDK 已安装到 $ADROID"
        echo "     使用的可执行环境变量可设为："
        echo "       export ANDROID_HOME=$ADROID"
        echo "       export JAVA_HOME=$ADROID/jdk/temurin-8.0.502+7   # x86_64"
        echo "       ndk 路径: $ADROID/ndk/<版本>  （与 scripts/build_apk.sh 的 NDK 对齐）"
    else
        echo "  ⚠️  Android SDK 部分组件下载失败，可稍后重跑本脚本（VUS_ANDROID=1 自动重试）"
    fi
    return $RC
}

# 在 VUS 成功安装后提供 Android SDK 选项（失败不阻断主流程）
if ! install_android_sdk; then
    echo "  （Android SDK 安装被跳过或失败，不影响 VUS 本身使用）"
fi

# 一键测试（仅在交互式终端中提示）
echo ""
if [ -e /dev/tty ] 2>&1; then
    echo "═══════════════════════════════════════"
    echo "  是否运行一键功能测试？"
    echo "  这将执行所有测试用例验证编译器功能"
    echo "  需要约 30-60 秒"
    echo "═══════════════════════════════════════"
    echo ""
    printf "运行测试? [y/N] "
    read -r RUN_TESTS </dev/tty 2>&1 || RUN_TESTS="no"
    case "$RUN_TESTS" in
        y|Y|yes|YES)
            ;;
        *)
            echo "跳过测试。"
            echo "可以随时手动运行: $INSTALL_DIR/vus test"
            RUN_TESTS="no"
            ;;
    esac
else
    RUN_TESTS="no"
fi

if [ "$RUN_TESTS" != "no" ]; then
        set +e
        echo ""
        echo "==== 运行 VUS 功能测试 ===="
        echo ""

        VUS="$INSTALL_DIR/vus"
        TESTS_DIR="$INSTALL_DIR/tests"
        PASS=0
        FAIL=0
        TOTAL=0

        # 测试 1: 基本编译
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 编译器版本..."
        if "$VUS" --version >/dev/null 2>&1; then
            echo "  ✅ 版本信息正确"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 版本信息错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 2: 帮助信息
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 帮助信息..."
        if "$VUS" --help >/dev/null 2>&1; then
            echo "  ✅ 帮助信息正确"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 帮助信息错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 3: 基本输出
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 基本输出 (test_hello)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_hello.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "Hello, World!"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 4: 变量
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 变量 (test_variables)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_variables.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "张三"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 5: 四则运算
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 四则运算 (test_arithmetic)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_arithmetic.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "7"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 6: 流程控制
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 流程控制 (test_control)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_control.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "大于"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 7: 函数
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 函数定义 (test_functions)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_functions.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "30"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 8: 比较运算符
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 比较运算符 (test_comparison)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_comparison.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "小于"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 9: 字符串拼接
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 字符串拼接 (test_string)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_string.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "字符串相等"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 10: 当循环
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 当循环 (test_while)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_while_count.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "55"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 11: 递归阶乘
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 递归阶乘 (test_factorial)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_factorial.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "720"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 12: 递归斐波那契
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 递归斐波那契 (test_fibonacci)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_fibonacci.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "21"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 13: 嵌套循环乘法表
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 嵌套循环 (test_nested_control)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_nested_control.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "9"; then
            echo "  ✅ 输出正确: 乘法表完整"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 14: 综合功能
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 综合功能 (test_comprehensive)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_comprehensive.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "平方"; then
            echo "  ✅ 综合功能正常"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 综合功能错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 15: 综合演示 (test_demo)
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 综合演示 (test_demo)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_demo.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "全部功能正常"; then
            echo "  ✅ 综合演示完成"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 综合演示错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 16: 示例程序
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 示例程序 (examples/hello)..."
        OUTPUT=$("$VUS" run "$INSTALL_DIR/examples/hello.vus" 2>&1)
        if echo "$OUTPUT" | grep -q "欢迎"; then
            echo "  ✅ 示例程序正确: a + b = 30"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 示例程序错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 17: 编译为 C 代码
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 编译为 C 代码..."
        BUILDDIR="$INSTALL_DIR/构建"
        mkdir -p "$BUILDDIR"
        if "$VUS" build --c-only "$TESTS_DIR/test_hello.vus" 2>&1; then
            echo "  ✅ C 编译成功"
            PASS=$((PASS + 1))
        else
            echo "  ❌ C 编译失败"
            FAIL=$((FAIL + 1))
        fi

        # 测试 18: 编译为可执行文件
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 编译为可执行文件..."
        if "$VUS" build --exe "$TESTS_DIR/test_hello.vus" 2>&1; then
            echo "  ✅ 可执行文件编译成功"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 可执行文件编译失败"
            FAIL=$((FAIL + 1))
        fi

        # 测试 19: 插件打包（需要 Python 3）
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 插件打包..."
        if command -v python3 >/dev/null 2>&1; then
            if python3 "$INSTALL_DIR/scripts/vux_plugin_manager.py" build "$INSTALL_DIR/examples/plugins/示例" >/dev/null 2>&1; then
                echo "  ✅ 插件打包成功"
                PASS=$((PASS + 1))
                # 清理临时文件
                rm -f "$INSTALL_DIR"/*.vux
            else
                echo "  ❌ 插件打包失败"
                FAIL=$((FAIL + 1))
            fi
        else
            echo "  ⚠️ 跳过（Python 3 未安装）"
        fi

        # 测试 20: 插件列表（需要 Python 3）
        if command -v python3 >/dev/null 2>&1; then
            TOTAL=$((TOTAL + 1))
            echo "测试 $TOTAL: 插件列表..."
            if python3 "$INSTALL_DIR/scripts/vux_plugin_manager.py" list >/dev/null 2>&1; then
                echo "  ✅ 插件列表正常"
                PASS=$((PASS + 1))
            else
                echo "  ❌ 插件列表错误"
                FAIL=$((FAIL + 1))
            fi
        fi

        # 测试 21: 编译为可执行文件并运行
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 编译为可执行文件并运行..."
        BUILDDIR="$INSTALL_DIR/构建"
        mkdir -p "$BUILDDIR"
        if "$VUS" build --exe "$TESTS_DIR/test_hello.vus" 2>&1 && [ -f "$BUILDDIR/test_hello" ]; then
            EXE_OUTPUT=$("$BUILDDIR/test_hello" 2>&1)
            if echo "$EXE_OUTPUT" | grep -q "Hello, World!"; then
                echo "  ✅ 可执行文件输出正确: $EXE_OUTPUT"
                PASS=$((PASS + 1))
            else
                echo "  ❌ 可执行文件输出错误: $EXE_OUTPUT"
                FAIL=$((FAIL + 1))
            fi
        else
            echo "  ❌ 可执行文件编译失败"
            FAIL=$((FAIL + 1))
        fi

        # 测试 22: vus lang list 命令
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: vus lang list 命令..."
        if "$VUS" lang list >/dev/null 2>&1; then
            echo "  ✅ vus lang list 执行成功"
            PASS=$((PASS + 1))
        else
            echo "  ❌ vus lang list 执行失败"
            FAIL=$((FAIL + 1))
        fi

        # 测试 23: vus vusx list 命令
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: vus vusx list 命令..."
        if "$VUS" vusx list >/dev/null 2>&1; then
            echo "  ✅ vus vusx list 执行成功"
            PASS=$((PASS + 1))
        else
            echo "  ❌ vus vusx list 执行失败"
            FAIL=$((FAIL + 1))
        fi

        # 测试 24: vus init 初始化模板
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: vus init 初始化模板..."
        INIT_DIR=$(mktemp -d 2>&1) || INIT_DIR=$(mktemp -d /tmp/vus_init.XXXXXX 2>&1)
        if (cd "$INIT_DIR" && echo "" | "$VUS" init) >/dev/null 2>&1; then
            if [ -f "$INIT_DIR/vus.json" ]; then
                echo "  ✅ vus.json 创建成功"
                PASS=$((PASS + 1))
            else
                echo "  ❌ vus.json 未创建"
                FAIL=$((FAIL + 1))
            fi
        else
            echo "  ❌ vus init 执行失败"
            FAIL=$((FAIL + 1))
        fi
        rm -rf "$INIT_DIR"

        # 测试 25: 中文错误信息
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 中文错误信息..."
        INVALID_FILE=$(mktemp --suffix=.vus 2>&1) || INVALID_FILE="/tmp/vus_test_$$.vus"
        echo '打印("Hello" + )' > "$INVALID_FILE"
        ERROR_OUTPUT=$("$VUS" build --c-only "$INVALID_FILE" 2>&1)
        if echo "$ERROR_OUTPUT" | grep -qE "错误|失败"; then
            echo "  ✅ 中文错误信息正确"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 未检测到中文错误信息: $ERROR_OUTPUT"
            FAIL=$((FAIL + 1))
        fi
        rm -f "$INVALID_FILE"

        # 测试 26: 调试模式 --debug
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 调试模式 (--debug)..."
        if "$VUS" run --debug "$TESTS_DIR/test_hello.vus" >/dev/null 2>&1; then
            echo "  ✅ 调试模式运行成功"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 调试模式运行失败"
            FAIL=$((FAIL + 1))
        fi

        # 总结
        echo ""
        echo "═══════════════════════════════════════"
        echo "  测试结果: $PASS/$TOTAL 通过, $FAIL 失败"
        echo "═══════════════════════════════════════"

        if [ "$FAIL" -eq 0 ]; then
            echo "  🎉 所有功能测试通过！"
        else
            echo "  ⚠️  部分测试失败，请检查输出"
        fi
        echo ""
    fi
    echo ""