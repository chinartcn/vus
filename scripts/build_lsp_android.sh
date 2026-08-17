#!/bin/bash
# VUS LSP Android 交叉编译脚本
#
# 目标：用 Android NDK 把 VUS（含 `vus lsp` 语言服务器）交叉编译为可在
#        Android 真机上运行的二进制，同时产出两种 ABI：
#         - arm64-v8a   （AArch64，对应 64 位设备）
#         - armeabi-v7a （EABI5，对应 32 位设备）
#
# 通过 NDK 的 clang（Bionic libc）编译，而非 linux-gnu 工具链——
# 后者产出的二进制无法在 Android（Bionic + /system/bin/linker）上运行。
#
# 用法：
#   bash scripts/build_lsp_android.sh                # 使用已安装 NDK
#   bash scripts/build_lsp_android.sh --get-ndk      # 自动下载 NDK 后再编译
#
# NDK 定位顺序：ANDROID_NDK_HOME → ANDROID_NDK_ROOT → 常见安装路径
# 产物输出到 release/android/{arm64-v8a,armeabi-v7a}/vus
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$SCRIPT_DIR"

NDK_VERSION="r25c"
NDK_URL="https://dl.google.com/android/repository/android-ndk-${NDK_VERSION}-linux.zip"

# ---------- 可选：自动下载 NDK ----------
if [ "$1" = "--get-ndk" ]; then
    if [ -z "$ANDROID_NDK_HOME" ]; then
        echo "== 未检测到 ANDROID_NDK_HOME，下载 NDK r${NDK_VERSION} ..."
        TMP=$(mktemp -d)
        curl -fsSL -o "$TMP/ndk.zip" "$NDK_URL"
        unzip -q "$TMP/ndk.zip" -d "$TMP"
        export ANDROID_NDK_HOME="$TMP/android-ndk-${NDK_VERSION}"
        echo "NDK 就绪: $ANDROID_NDK_HOME"
    fi
fi

# ---------- 定位 NDK ----------
NdkHome="${ANDROID_NDK_HOME:-$ANDROID_NDK_ROOT}"
if [ -z "$NdkHome" ]; then
    for p in /opt/android-ndk-* "$HOME/Android/Sdk/ndk/"* /usr/local/android-ndk-*; do
        if [ -d "$p" ]; then
            NdkHome="$p"
            break
        fi
    done
fi
if [ -z "$NdkHome" ] || [ ! -d "$NdkHome" ]; then
    echo "错误: 未找到 Android NDK。"
    echo "  方式一: export ANDROID_NDK_HOME=/path/to/ndk 后重跑本脚本"
    echo "  方式二: bash scripts/build_lsp_android.sh --get-ndk（自动下载）"
    exit 1
fi
TOOLS="$NdkHome/toolchains/llvm/prebuilt/linux-x86_64/bin"
if [ ! -x "$TOOLS/aarch64-linux-android21-clang" ]; then
    echo "错误: 在 $NdkHome 未找到 NDK clang（期望 linux-x86_64 预编译工具链）。"
    exit 1
fi

echo "== 使用 NDK: $NdkHome"

# ---------- 清理中间目录 ----------
rm -rf build-arm64 build-arm32
OUT="$SCRIPT_DIR/release/android"
mkdir -p "$OUT/arm64-v8a" "$OUT/armeabi-v7a"

# ---------- 构建 arm64-v8a ----------
echo "== 构建 arm64-v8a ..."
make PY_DEF= PY_INC= PY_LD= \
     BUILD_DIR=build-arm64 \
     CC="$TOOLS/aarch64-linux-android21-clang" \
     vus >/dev/null
mv vus "$OUT/arm64-v8a/vus"

# ---------- 构建 armeabi-v7a ----------
echo "== 构建 armeabi-v7a ..."
make PY_DEF= PY_INC= PY_LD= \
     BUILD_DIR=build-arm32 \
     CC="$TOOLS/armv7a-linux-androideabi21-clang" \
     vus >/dev/null
mv vus "$OUT/armeabi-v7a/vus"

# ---------- 恢复宿主 x86-64 版 vus ----------
make >/dev/null

# ---------- 清理中间目录 ----------
rm -rf build-arm64 build-arm32

echo ""
echo "== 完成。产物:"
echo "  $OUT/arm64-v8a/vus"
echo "  $OUT/armeabi-v7a/vus"
file "$OUT/arm64-v8a/vus" "$OUT/armeabi-v7a/vus"