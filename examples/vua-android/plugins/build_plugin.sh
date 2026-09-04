#!/usr/bin/env bash
# build_plugin.sh — 打包示例 DEX 逻辑拓展插件（sample.dex + sha256）
#
# 产物: examples/vua-android/plugins/dist/sample.dex (+ .sha256)
# 由 build_apk.sh 复制进 APK assets/plugins/，MainActivity 启动时释放到
# filesDir/plugins/，供 ExtensionLoader 动态加载（VUS: 拓展_调用）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"          # examples/vua-android
SDK="${SDK:-/workspace/android}"
BT="$SDK/bt/android-14"
AJ="$SDK/platforms/android-34/android.jar"
JAVA8="/root/.local/share/mise/installs/java/temurin-8.0/bin/javac"

WORK="$ROOT/plugins/build"
SRC_IFACE="$ROOT/app/src/main/java/com/vus/android/VusExtension.java"
SRC_PLUGIN="$ROOT/plugins/sample/java/com/vus/plugins/SamplePlugin.java"
OUT="$ROOT/plugins/dist"

rm -rf "$WORK"; mkdir -p "$WORK/classes" "$OUT"
"$JAVA8" -cp "$AJ" -d "$WORK/classes" "$SRC_IFACE" "$SRC_PLUGIN"
"$BT/d8" --release --min-api 21 --output "$OUT" "$WORK/classes"/com/vus/android/*.class "$WORK/classes"/com/vus/plugins/*.class
mv "$OUT/classes.dex" "$OUT/sample.dex"
# sha256 校验文件：ExtensionLoader 检测到同名 .sha256 时强制校验
(cd "$OUT" && sha256sum sample.dex | awk '{print $1}' > sample.dex.sha256)

echo "== 完成: $OUT/sample.dex ($(stat -c%s "$OUT/sample.dex") bytes) =="