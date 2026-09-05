#!/usr/bin/env bash
# build_apk.sh — 用 NDK + build-tools 手工编译 VUS APK（无需 Gradle）、完整构建链
# 用法: scripts/build_apk.sh [--abi arm64-v8a|armeabi-v7a|x86_64|all]
#                            [--ndk /workspace/android/ndk] [--sdk /workspace/android]
#
# 依据反馈（docs/VUS开发体验反馈-安卓计算器APK.md §3）实现：
#   P3/3.1 JNI 桥自动生成 + 符号核对：jni_bridge.c 由 scripts/gen_jni_bridge.py
#         从 Java native 声明生成（符号随包名自动对齐），构建后 nm 校验 Java_* 导出。
#   P4/3.3 JDK 版本探测与降级：优先 JDK8；JDK9+ 自动加 --release 8（d8/build-tools31 只认老 class）。
#         --abi all 一次产出 arm64-v8a + armeabi-v7a + x86_64 打进同一 APK。
#   P5/3.2 native 单一真源：rt/ 唯一，jni/ 不再留存 vua/libvus_rt/yyjson 副本
#         （历史上 jni/ 副本与 rt/ 分叉导致改错源文件），构建全部从 rt/ 编译。
#
# 产物: <工作目录>/dist/VUS.apk  （可安装、已签名）
set -euo pipefail

# ---------- 参数 ----------
ABI="${ABI:-all}"
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
VUA_SRC="${VUA_SRC:-$ROOT/../../rt}"               # native 运行时唯一真源（P5）
GEN_BRIDGE="${GEN_BRIDGE:-$ROOT/../../scripts/gen_jni_bridge.py}"
BRIDGE="$ROOT/jni/jni_bridge.c"                    # 生成产物（P3，勿手改）
# 可选 .vaz 扩展包（插件式：目录含 vaz.json 即启用；逻辑依赖已在 vua_test.vus 用 导入 "包名" 声明）
VAZ="${VAZ:-$TESTDATA_SRC/vaz/common-controls}"

# ---------- 工具 ----------
BT="$SDK/bt/android-14"
AJ="$SDK/platforms/android-34/android.jar"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"

if [ "$ABI" = "all" ]; then
    ABIS=(arm64-v8a armeabi-v7a x86_64)
else
    ABIS=("$ABI")
fi

WORK="$ROOT/build/tmp"
STM="$ROOT/build/staged"

printf '== VUS APK build ==\n'
printf '  ABI=%s  NDK=%s  SDK=%s\n' "$ABI" "$NDK" "$SDK"
for a in "${ABIS[@]}"; do mkdir -p "$WORK/classes" "$STM"; done

# ---------- JDK 探测与降级（P4/3.3） ----------
# d8（build-tools 31）只认 class 文件 <= Java 11（实测 JDK 8 最稳；新版 d8 兼容差）。
# 策略：有 JDK8 用 JDK8；否则用任意 JDK9+ 并加 --release 8 编出 class 52。
find_javac() {
    local c
    for c in \
        "${JAVA_HOME:-}/bin/javac" \
        /root/.local/share/mise/installs/java/temurin-8.0/bin/javac \
        /usr/lib/jvm/java-8-*/bin/javac \
        /usr/lib/jvm/temurin-8*/bin/javac \
        /usr/lib/jvm/adoptopenjdk-8*/bin/javac ; do
        [ -n "$c" ] && [ -x "$c" ] && echo "$c" && return 0
    done
    command -v javac >/dev/null 2>&1 && command -v javac && return 0
    return 1
}
JAVAC="$(find_javac)" || { echo "错误: 找不到 javac（设置 JAVA_HOME）" >&2; exit 1; }
JAVA_VER="$("$JAVAC" -version 2>&1 | tr -d '"' | awk '{print $2}')"
JAVA_MAJOR="$(echo "$JAVA_VER" | awk -F. '{print ($1==1)?$2:$1}')"
JAVA_MAJOR="${JAVA_MAJOR:-0}"
# 编译选项：JDK8 用 -source/target；JDK9+ 用 --release（生成 class 52，d8-31 可解）
if [ "$JAVA_MAJOR" -ge 9 ]; then
    JAVAC_ARGS=(--release 8)
    echo "[jdk] $JAVAC ($JAVA_VER) 检测到 JDK9+，自动 --release 8 降级（d8 兼容）"
else
    JAVAC_ARGS=(-source 8 -target 8)
    echo "[jdk] $JAVAC ($JAVA_VER) JDK8，直接 -source/-target 8"
fi

# ---------- 0. 同步资产（vua 页面 + 控件表以 testdata 为单一来源） ----------
mkdir -p "$ASSETS"
cp "$TESTDATA_SRC"/vua_home.vua "$TESTDATA_SRC"/vua_settings.vua \
   "$TESTDATA_SRC"/vua_logic.vua "$TESTDATA_SRC"/vua_class.vua \
   "$TESTDATA_SRC"/vua_advanced.vua "$TESTDATA_SRC"/vua_controls.json "$ASSETS/"
cp "$TESTDATA_SRC"/*.jpg "$ASSETS"/ 2>/dev/null || true
# 示例 DEX 逻辑拓展插件（ExtensionLoader 加载，检出 .sha256 即强制校验）
if [ -d "$ROOT/plugins/dist" ]; then
  mkdir -p "$ASSETS/plugins"
  cp "$ROOT/plugins/dist"/sample.dex "$ROOT/plugins/dist"/sample.dex.sha256 "$ASSETS/plugins/" 2>/dev/null || true
fi

# ---------- 0b. 可选: .vaz 控件模板展开（无包则跳过，核心分层不变） ----------
VUS_BIN="${VUS_BIN:-$ROOT/../../vus}"
if [ -f "$VAZ/vaz.json" ]; then
  echo "[0b] 展开 .vaz 控件模板: $VAZ"
  "$VUS_BIN" vaz expand "$ASSETS" -v "$VAZ"
fi
# 布局模板包（侧边栏布局等，pkg 含 vaz.json 即启用；与逻辑包先后展开互不干扰）
LAYOUT_VAZ="${LAYOUT_VAZ:-$TESTDATA_SRC/vaz/layout}"
if [ -f "$LAYOUT_VAZ/vaz.json" ]; then
  echo "[0b] 展开 .vaz 布局模板: $LAYOUT_VAZ"
  "$VUS_BIN" vaz expand "$ASSETS" -v "$LAYOUT_VAZ"
fi

# ---------- 0c. 生成 vus_app.c（vua_test.vus 内 导入 "通用控件包" 由编译器自动展开） ----------
if [ ! -f "$VUS_C" ] || [ "$TESTDATA_SRC/vua_test.vus" -nt "$VUS_C" ]; then
  echo "[0c/6] 由 vua_test.vus 生成 vus_app.c"
  rm -f "$TESTDATA_SRC/构建/vua_test.c"
  ( cd "$TESTDATA_SRC" && "$VUS_BIN" build --c-only vua_test.vus >/dev/null 2>&1 )
  cp "$TESTDATA_SRC"/构建/vua_test.c "$VUS_C"
  # generator 现生成 main(int argc, char** argv) + vus_cli_init(argc, argv)；
  # APK 场景按 jni_bridge 的 `extern int vus_main(void)` 约定改名，并清空 CLI 参数。
  sed -i -e 's/^int main(.*)/int vus_main(void)/' \
         -e 's/vus_cli_init(argc, argv)/vus_cli_init(0, NULL)/' "$VUS_C"
fi

# ---------- 0d. JNI 桥自动生成 + rt 单一真源校验（P3/P5） ----------
echo "[0d] 生成 jni_bridge.c（由 Java native 声明自动生成）"
mkdir -p "$(dirname "$BRIDGE")"
python3 "$GEN_BRIDGE" --java "$JAVA_SRC" --class VuaBridge --bridge "$BRIDGE" 2>&1
# P5 校验：jni/ 里任何残留的旧 rt 副本都不许再出现（历史上 vua.c/libvus_rt.c 两副本分叉，
# 改错源文件；现构建全部从 rt/ 编译，jni/ 只留生成产物 jni_bridge.c）
for f in vua.c vua.h libvus_rt.c libvus_rt.h; do
  if [ -f "$ROOT/jni/$f" ]; then
    echo "[0d] 清理残留旧副本 jni/$f —— rt/ 是唯一真源" >&2
    rm -f "$ROOT/jni/$f"
  fi
done

# ---------- 1. 编译 native .so（rt/ 直接编译，多 ABI 循环） ----------
for ABI in "${ABIS[@]}"; do
  echo "[1/6] 编译 native ($ABI) -> libvus_app.so"
  case "$ABI" in
    arm64-v8a) CC="$TC/aarch64-linux-android21-clang" ;;
    armeabi-v7a) CC="$TC/armv7a-linux-androideabi21-clang" ;;
    x86_64) CC="$TC/x86_64-linux-android21-clang" ;;
    *) echo "未知 ABI: $ABI"; exit 1 ;;
  esac
  LS="$ROOT/build/libs/$ABI"
  mkdir -p "$LS"
  "$CC" -O2 -std=c11 -fPIC \
    -I"$ROOT/jni" -I"$VUA_SRC" -I"$VUA_SRC/easylogger/inc" \
    "$ROOT/jni/jni_bridge.c" \
    "$VUA_SRC/vua.c" \
    "$VUA_SRC/libvus_rt.c" \
    "$VUA_SRC/vus_coro.c" \
    "$VUA_SRC/easylogger/src/elog.c" \
    "$VUA_SRC/easylogger/src/elog_utils.c" \
    "$VUA_SRC/elog_port.c" \
    "$VUS_C" \
    "$VUA_SRC/yyjson/yyjson.c" \
    -shared -o "$LS/libvus_app.so" -llog -lm

  # ---- P3 符号核对：Java 声明 必须 全部在 .so 里导出 ----
  if ! python3 "$GEN_BRIDGE" --expect --java "$JAVA_SRC" --class VuaBridge \
     > "$WORK/jni_expect_$ABI.txt" 2>/dev/null; then
    echo "[1] JNI 声明提取失败" >&2; exit 1
  fi
  NM="${TC}/llvm-nm"
  [ -x "$NM" ] || NM="nm"
  # nm 输出先落临时文件再逐个 grep：`nm | grep -q` 在 grep 命中早期符号后退出，
  # nm 收到 SIGPIPE 中断、后续符号输出丢失 → 误报"缺少导出符号"（反馈 3.1）。
  "$NM" -D --defined-only "$LS/libvus_app.so" > "$WORK/nm_$ABI.txt" 2>/dev/null || true
  missing=0
  while read -r sym; do
    [ -z "$sym" ] && continue
    if ! grep -q " T $sym$" "$WORK/nm_$ABI.txt"; then
      echo "  [JNI 校验] 缺少导出符号: $sym" >&2
      missing=1
    fi
  done < "$WORK/jni_expect_$ABI.txt"
  if [ "$missing" -eq 0 ]; then
      echo "  [JNI 校验] $ABI 导出 $(wc -l < "$WORK/jni_expect_$ABI.txt") 个 Java_* 符号全部通过"
  else
      echo "  [JNI 校验] $ABI 导出 $(wc -l < "$WORK/jni_expect_$ABI.txt") 个 Java_* 符号(缺失!)"
  fi
  [ "$missing" -eq 0 ] || { echo "JNI 符号校验失败，中止" >&2; exit 1; }
done

# ---------- 2. 编译 Java -> classes.dex ----------
echo "[2/6] 编译 Java -> classes.dex"
"$JAVAC" "${JAVAC_ARGS[@]}" -cp "$AJ" -d "$WORK/classes" "$JAVA_SRC"/*.java 2>&1
rm -rf "$WORK/dex"; mkdir -p "$WORK/dex"
# dex 工具：SDK 下已有 R8 jar 则优先（build-tools 自带 d8 8.2.2-dev 对此工程的嵌套类
# 有 NPE 崩溃 bug，R8 正式版正常；无 jar 时回退 d8）。--lib android.jar 为 desugar
# lambda/默认接口方法提供 java.lang.Runnable 等平台类型。
# 探测放宽：文件名可能是 r8.jar/r8-8.x.jar，深度可到 4+（cmdline-tools/latest/lib）（反馈 3.2）
R8_JAR_FOUND="$(find "$SDK" -maxdepth 5 -name 'r8*.jar' 2>/dev/null | head -1)"
if [ -n "$R8_JAR_FOUND" ] && command -v java >/dev/null 2>&1; then
  echo "[2/6] 用 R8 jar 做 dex: $R8_JAR_FOUND"
  java -cp "$R8_JAR_FOUND" com.android.tools.r8.D8 --release --min-api 21 --lib "$AJ" \
      --output "$WORK/dex" "$WORK/classes"/com/vus/android/*.class
else
  "$BT/d8" --release --min-api 21 --lib "$AJ" --output "$WORK/dex" "$WORK/classes"/com/vus/android/*.class
fi

# ---------- 3. 编译资源并打包 ----------
echo "[3/6] aapt 编译资源并打包"
rm -rf "$STM"; mkdir -p "$STM" "$STM/lib" "$STM/assets"
# classes.dex 必须放在 APK 根目录，否则安装时报 "code is missing"
cp "$WORK/dex/classes.dex" "$STM/classes.dex"
# 多 ABI：全部打进 lib/<abi>/（P4）
for ABI in "${ABIS[@]}"; do
  mkdir -p "$STM/lib/$ABI"
  cp "$ROOT/build/libs/$ABI/libvus_app.so" "$STM/lib/$ABI/"
done
cp "$ASSETS"/vua_home.vua "$ASSETS"/vua_settings.vua "$ASSETS"/vua_logic.vua \
   "$ASSETS"/vua_class.vua "$ASSETS"/vua_advanced.vua "$ASSETS"/vua_controls.json "$STM/assets/"
cp "$ASSETS"/*.jpg "$ASSETS"/*.png "$STM/assets/" 2>/dev/null || true
cp -r "$ASSETS/plugins" "$STM/assets/" 2>/dev/null || true

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