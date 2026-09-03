#!/usr/bin/env bash
# build_apk.sh — 用 NDK + build-tools 手工编译 VUS APK（无需 Gradle）
# 用法: scripts/build_apk.sh [--abi arm64-v8a] [--ndk /workspace/android/ndk] [--sdk /workspace/android]
#
# 产物: <工作目录>/dist/VUS.apk  （可安装、已签名）
set -euo pipefail

# ---------- 参数 ----------
ABI="${ABI:-arm64-v8a}"
NDK="${NDK:-/workspace/android/ndk}"
SDK="${SDK:-/workspace/android}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"          # examples/vua-android
OUT_DIR="$ROOT/dist"

JAVA_SRC="$ROOT/app/src/main/java/com/vus/android"
# 资产以仓库 testdata/ 为单一可信来源（.vua/.json），每次构建前自动同步，
# 避免 app/src/main/assets 里的副本过期导致 APK 打包到旧页面。
TESTDATA_SRC="${TESTDATA_SRC:-$ROOT/../../testdata}"
ASSETS="$ROOT/app/src/main/assets"
MANIFEST="$ROOT/AndroidManifest.xml"

VUS_C="${VUS_C:-$ROOT/build/vus_app.c}"            # 由 vua_test.vus 生成
VUA_SRC="${VUA_SRC:-$ROOT/../../rt}"               # rt/ 目录(vua.c/libvus_rt.c/vus_coro.c/yyjson)

# ---------- 工具 ----------
BT="$SDK/bt/android-14"
AJ="$SDK/platforms/android-34/android.jar"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
JAVAC="${JAVA_HOME:+$JAVA_HOME/bin/}javac"

WORK="$ROOT/build/tmp"
STM="$ROOT/build/staged"
LS="$ROOT/build/libs/$ABI"

printf '== VUS APK build ==\n'
printf '  ABI=%s  NDK=%s  SDK=%s\n' "$ABI" "$NDK" "$SDK"
mkdir -p "$WORK" "$STM" "$LS"

# ---------- 0. 同步资产 + 生成 vus_app.c（保证页面与逻辑一致） ----------
VUS_BIN="${VUS_BIN:-$ROOT/../../vus}"
if [ ! -f "$VUS_C" ] || [ "$TESTDATA_SRC/vua_test.vus" -nt "$VUS_C" ]; then
  echo "[0/6] 由 $TESTDATA_SRC/vua_test.vus 生成 vus_app.c"
  ( cd "$TESTDATA_SRC" && "$VUS_BIN" build --c-only vua_test.vus >/dev/null 2>&1 )
  cp "$TESTDATA_SRC"/构建/vua_test.c "$VUS_C"
  sed -i 's/^int main(void)/int vus_main(void)/' "$VUS_C"
fi
cp "$TESTDATA_SRC"/vua_home.vua "$TESTDATA_SRC"/vua_settings.vua \
   "$TESTDATA_SRC"/vua_logic.vua "$TESTDATA_SRC"/vua_controls.json "$ASSETS/"
cp "$TESTDATA_SRC"/*.jpg "$ASSETS"/ 2>/dev/null || true

# ---------- 1. 编译 native .so ----------
echo "[1/6] 编译 native ($ABI) -> libvus_app.so"
case "$ABI" in
  arm64-v8a) CC="$TC/aarch64-linux-android21-clang" ;;
  armeabi-v7a) CC="$TC/armv7a-linux-androideabi21-clang" ;;
  x86_64) CC="$TC/x86_64-linux-android21-clang" ;;
  *) echo "未知 ABI: $ABI"; exit 1 ;;
esac
"$CC" -O2 -std=c11 -fPIC \
  -I"$ROOT/jni" -I"$ROOT/jni/yyjson" -I"$ROOT/jni/easylogger/inc" -I"$VUA_SRC" \
  "$ROOT/jni/jni_bridge.c" \
  "$VUA_SRC/vua.c" \
  "$VUA_SRC/libvus_rt.c" \
  "$VUA_SRC/vus_coro.c" \
  "$ROOT/jni/easylogger/src/elog.c" \
  "$ROOT/jni/easylogger/src/elog_utils.c" \
  "$ROOT/jni/easylogger/elog_port.c" \
  "$VUS_C" \
  "$VUA_SRC/yyjson/yyjson.c" \
  -shared -o "$LS/libvus_app.so" -llog -lm

# ---------- 1b. 同步 rt/vua.* 到 jni（保证声明最新） ----------
cp "$VUA_SRC/vua.c" "$VUA_SRC/vua.h" "$ROOT/jni/"

# ---------- 2. 编译 Java -> classes.dex ----------
echo "[2/6] 编译 Java -> classes.dex"
rm -rf "$WORK/classes"; mkdir -p "$WORK/classes"
JAVA8="/root/.local/share/mise/installs/java/temurin-8.0/bin/javac"
"$JAVA8" -cp "$AJ" -d "$WORK/classes" "$JAVA_SRC"/*.java 2>&1
rm -rf "$WORK/dex"; mkdir -p "$WORK/dex"
"$BT/d8" --release --min-api 21 --output "$WORK/dex" "$WORK/classes"/com/vus/android/*.class

# ---------- 3. 编译资源并打包 ----------
echo "[3/6] aapt 编译资源并打包"
rm -rf "$STM"; mkdir -p "$STM" "$STM/lib" "$STM/assets"
# classes.dex 必须放在 APK 根目录，否则安装时报 "code is missing"
cp "$WORK/dex/classes.dex" "$STM/classes.dex"
mkdir -p "$STM/lib/$ABI"; cp "$LS/libvus_app.so" "$STM/lib/$ABI/"
cp "$ASSETS"/vua_home.vua "$ASSETS"/vua_settings.vua "$ASSETS"/vua_logic.vua "$ASSETS"/vua_controls.json "$STM/assets/"
cp "$ASSETS"/*.jpg "$ASSETS"/*.png "$STM/assets/" 2>/dev/null || true

# 用 aapt 编译 manifest + resources 生成资源表
if [ -d "$ROOT/app/src/main/res" ]; then RES_SW=" -S $ROOT/app/src/main/res"; else RES_SW=""; fi
"$BT/aapt" package -f -M "$MANIFEST" $RES_SW -I "$AJ" -F "$WORK/base.apk"

# ---------- 4. 组装 APK ----------
echo "[4/6] 组装未签名 APK"
UNSIGNED="$WORK/vus-unsigned.apk"
cp "$WORK/base.apk" "$UNSIGNED"
( cd "$STM" && zip -q -r "$UNSIGNED" classes.dex lib assets )

# ---------- 5. zipalign ----------
echo "[5/6] zipalign"
ALIGNED="$WORK/vus-aligned.apk"
"$BT/zipalign" -f 4 "$UNSIGNED" "$ALIGNED"

# ---------- 6. apksigner 签名 ----------
echo "[6/6] 签名"
mkdir -p "$ROOT/build"
KEY="$ROOT/build/vus-test.keystore"
if [ ! -f "$KEY" ]; then
  keytool -genkeypair -keystore "$KEY" -alias vus -storepass vus12345 \
    -keypass vus12345 -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=VUS Test, OU=VUS, O=VUS, L=CN, C=CN" 2>/dev/null
fi
mkdir -p "$OUT_DIR"
"$BT/apksigner" sign --ks "$KEY" --ks-pass pass:vus12345 --out "$OUT_DIR/VUS.apk" "$ALIGNED"

echo "== 完成: $OUT_DIR/VUS.apk =="
ls -lh "$OUT_DIR/VUS.apk"