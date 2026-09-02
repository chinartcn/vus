#!/usr/bin/env bash
# build_apk.sh — 用 NDK + build-tools 手工编译 VUS APK（无需 Gradle）
# 用法: scripts/build_apk.sh [--abi arm64-v8a] [--ndk /opt/ndk] [--sdk /opt/android]
#
# 产物: <工作目录>/dist/VUS.apk  （可安装、已签名）
set -euo pipefail

# ---------- 参数 ----------
ABI="${ABI:-arm64-v8a}"
NDK="${NDK:-/opt/ndk}"
SDK="${SDK:-/opt/android}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"          # examples/vua-android
OUT_DIR="$ROOT/dist"

JAVA_SRC="$ROOT/app/src/main/java/com/vus/android"
JNI_SRC="$ROOT/app/src/main/jniLibs/$ABI"
ASSETS="$ROOT/app/src/main/assets"
MANIFEST="$ROOT/AndroidManifest.xml"

VUS_C="${VUS_C:-$ROOT/build/vus_app.c}"            # 由 vua_test.vus 生成
VUA_SRC="${VUA_SRC:-$ROOT/../../rt}"               # rt/ 目录(vua.c/libvus_rt.h/vus_rt_shim.c/yyjson)
VUS_RT_SHIM="$VUA_SRC/vus_rt_shim.c"

# ---------- 工具 ----------
BT="$SDK/bt"
AJ="$SDK/platforms/android-34/android-34/android.jar"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
TOOLS="$BT/aapt $BT/d8 $BT/zipalign $BT/apksigner"
JAVAC="${JAVA_HOME:+$JAVA_HOME/bin/}javac"

WORK="$ROOT/build/tmp"
STM="$ROOT/build/staged"
LS="$ROOT/build/libs/$ABI"

printf '== VUS APK build ==\n'
printf '  ABI=%s  NDK=%s  SDK=%s\n' "$ABI" "$NDK" "$SDK"
mkdir -p "$WORK" "$STM" "$LS"

# ---------- 1. 编译 native .so ----------
echo "[1/6] 编译 native ($ABI) -> libvus_app.so"
COMPILER="$TC"
case "$ABI" in
  arm64-v8a) CC="$TC/aarch64-linux-android21-clang" ;;
  armeabi-v7a) CC="$TC/armv7a-linux-androideabi21-clang" ;;
  x86_64) CC="$TC/x86_64-linux-android21-clang" ;;
  *) echo "未知 ABI: $ABI"; exit 1 ;;
esac
"$CC" -O2 -std=c11 -fPIC \
  -I"$ROOT/jni" -I"$ROOT/jni/yyjson" -I"$VUA_SRC" \
  "$ROOT/jni/jni_bridge.c" \
  "$VUA_SRC/vua.c" \
  "$VUS_RT_SHIM" \
  "$VUS_C" \
  "$VUA_SRC/yyjson/yyjson.c" \
  -shared -o "$LS/libvus_app.so" -llog -lm

# ---------- 2. 编译 Java -> classes.dex ----------
echo "[2/6] 编译 Java -> classes.dex"
rm -rf "$WORK/classes"; mkdir -p "$WORK/classes"
"$JAVAC" --release 8 -cp "$AJ" -d "$WORK/classes" "$JAVA_SRC"/*.java 2>/dev/null || true
rm -rf "$WORK/dex"; mkdir -p "$WORK/dex"
"$BT/d8" --release --min-api 21 --output "$WORK/dex" "$WORK/classes"/com/vus/android/*.class

# ---------- 3. aapt2 编译资源并打包 ----------
echo "[3/6] aapt 编译资源并打包"
rm -rf "$STM"; mkdir -p "$STM/classes" "$STM/lib" "$STM/assets"
cp "$WORK/dex/classes.dex" "$STM/classes/"
mkdir -p "$STM/lib/$ABI"; cp "$LS/libvus_app.so" "$STM/lib/$ABI/"
cp "$ASSETS"/vua_home.vua "$ASSETS"/vua_settings.vua "$ASSETS"/vua_controls.json "$STM/assets/"

# 先用 aapt 生成资源编译(binary xml 可选: 直接原样打包 manifest)
BASE_APK="$WORK/base.apk"
"$BT/aapt" package -f -M "$MANIFEST" \
  -S "$ROOT/app/src/main/res" \
  -I "$AJ" \
  -F "$BASE_APK" 2>/dev/null || {

  # 无 res 目录时，用最小命令打包(manifest 原样)
  "$BT/aapt" package -f -M "$MANIFEST" -I "$AJ" -F "$BASE_APK"
}

# ---------- 4. 组装 APK ----------
echo "[4/6] 组装未签名 APK"
UNSIGNED="$WORK/vus-unsigned.apk"
cp "$BASE_APK" "$UNSIGNED"
( cd "$STM" && zip -q -r "$UNSIGNED" classes lib assets )

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