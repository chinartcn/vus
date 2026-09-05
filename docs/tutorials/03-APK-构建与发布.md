# APK 构建与发布

> 手把手讲 VUS Android 示例工程的完整构建链：`examples/vua-android/scripts/build_apk.sh`。
> 该脚本**不用 Gradle**，由 NDK 交叉编译 native + javac/R8 编译 Java + aapt/zip/apksigner 打包签名。
> 读完你能改完代码立刻出 APK，并知道构建失败时去哪儿找原因。

---

## 1. 前置条件

| 工具 | 位置（默认） | 检查 |
|------|--------------|------|
| NDK（含 clang 交叉工具链） | `/workspace/android/ndk` | `ls $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin` |
| Android SDK（platforms + build-tools） | `/workspace/android` | 内含 `platforms/android-34/android.jar`、`bt/android-14/{aapt,d8,zipalign,apksigner}` |
| JDK（8 或 9+） | `JAVA_HOME` 或 PATH | `javac -version`；脚本对 JDK9+ 自动加 `--release 8` |
| 仓库产物 `vus` 编译器 | `make` 构建 | `make`（仓库根目录） |

> 构建脚本通过环境变量覆盖：`NDK=`、`SDK=`、`ABI=`（`arm64-v8a|armeabi-v7a|x86_64|all`，默认 `all`）。

---

## 2. 一键构建

```bash
cd vus
make                                   # 先出编译器 vus（含 vaz 展开能力）

# 只打一种 ABI（最快，本机/模拟器常用 x86_64 或 arm64-v8a）
ABI=x86_64 bash examples/vua-android/scripts/build_apk.sh

# 或三个 ABI 全打（教程默认）
bash examples/vua-android/scripts/build_apk.sh

# 产物
ls -lh examples/vua-android/dist/VUS.apk
```

> `ABI=all` 会把 `lib/<abi>/libvus_app.so` 三个全打进 APK（约 322KB）。
> 成功标志：日志末尾 `== 完成: .../dist/VUS.apk ==`。

---

## 3. 构建流水线逐段解读

`build_apk.sh` 共 6 步（日志带编号 `[0~6]`）：

### 0. 资产同步与代码生成

```
[0]   assets ← testdata（vua_*.vua + vua_controls.json 以 testdata 为单一真相源）
[0b]  展开 .vaz 扩展包（testdata/vaz/common-controls：搜索条/星级评分等复合控件）
[0c]  vua_test.vus → vus_app.c（生成器 build --c-only；builtin main() 改名 vus_main()
     并清空 CLI 参数，适配 JNI 入口约定）
[0d]  由 Java native 声明自动生成 jni/jni_bridge.c（scripts/gen_jni_bridge.py，
     符号随包名对齐，勿手改）
```

> **改页面/逻辑后的刷新方式**：只改 `.vua`/`.vus` 时，重建即可；
> `vua_test.vus` 变新会重跑 [0c]，页面文件每轮都同步。

### 1. native 编译（每个 ABI）

```
$NDK/.../aarch64-linux-android21-clang -O2 -std=c11 -fPIC \
    jni/jni_bridge.c rt/vua.c rt/libvus_rt.c rt/vus_coro.c \
    rt/easylogger/src/elog*.c rt/elog_port.c build/vus_app.c rt/yyjson/yyjson.c \
    -shared -o build/libs/<abi>/libvus_app.so -llog -lm
```

随后 **JNI 符号校验**：`nm -D` 逐个核对脚本提取的 `Java_*` 导出符号，
与 Java 侧 `native` 声明一一对应，缺漏即中止（防止包名/签名漂移导致的运行期
`UnsatisfiedLinkError`）。

### 2. Java → classes.dex

```
javac --release 8 -cp android.jar -d build/tmp/classes  com/vus/android/*.java
java -cp <r8jar> com.android.tools.r8.D8 --lib android.jar --min-api 21 \
     --output build/tmp/dex build/tmp/classes/com/vus/android/*.class
```

> **为什么用 R8 jar 而不是 d8**：build-tools 自带的 d8 8.2.2-dev 在此工程的部分
> 嵌套类上会内部 NPE 崩溃；R8 正式版（`/workspace/android/r8lib/r8-*.jar`，
> 从 Google Maven 下载，`SDK` 目录下探测到即自动采用）一切正常。
> `--lib android.jar` 是为 desugar lambda 提供 `java.lang.Runnable` 等平台类型（必需）。

### 3~4. 资源 + 组装

- aapt 编译 Manifest/资源 → `base.apk`
- 组装：`classes.dex` 进根目录、`lib/<abi>/*.so`、`assets/`（页面/控件表/图片/plugins）打包

### 5~6. 对齐 + 签名

```
zipalign -f 4                    # 对齐到 4 字节（发布要求）
apksigner sign --ks build/vus-test.keystore
```

测试签名自动生成（首次构建 `keytool` 建 `vus-test.keystore`，口令 `vus12345`）。
安装到手机/模拟器：

```bash
adb install -r examples/vua-android/dist/VUS.apk
```

---

## 4. 改一处，重出一包（日常开发流）

| 想改什么 | 改哪里 | 重建 |
|----------|--------|------|
| 页面布局/文案 | `testdata/vua_*.vua` | 全量构建 |
| 事件逻辑/网络 | `testdata/vua_test.vus` | 全量构建（触发 [0c] 重新生 C） |
| 新增控件类型 | `vua_controls.json` + `VuaRenderer.java` | 全量构建 |
| 网络/文件能力 | `VuaBridge.java`；native 侧 `rt/libvus_rt.c` | 全量构建 |
| 图片加载 | `ImageLoader.java` | 全量构建 |
| JNI 签名变化 | 改 `VuaBridge.java` 的 native 声明 | 全量构建（[0d] 重新生成桥） |

改完 Java 但只验证编译（不出包）：

```bash
javac --release 8 -cp /workspace/android/platforms/android-34/android.jar \
      -d /tmp/out examples/vua-android/app/src/main/java/com/vus/android/*.java
```

桌面验证语言/逻辑（不走安卓，快）：

```bash
./vus run testdata/vua_test.vus    # 或 tests/ 下任意用例
```

---

## 5. 常见构建故障

| 现象 | 原因 | 处理 |
|------|------|------|
| `[0d] gen_jni_bridge` 报"有 native 声明但没实现" | Java 新加了 `native` 方法 | 在 `scripts/gen_jni_bridge.py` 的 `KNOWN_BODIES` 补方法体（或去掉声明） |
| `[1] 缺少导出符号: Java_...` | JNI 桥生成与 Java 声明不一致 | 重跑 [0d]；若改了包名/方法名请先重建 |
| d8 段 NPE / `String.length null` | 用了不兼容的 d8 | 下载 R8 正式版 jar 到 SDK 下（见 §3 第 2 步） |
| javac 报 `-source 8 已过时` | JDK 9+ 提示 | 无害；脚本自动 `--release 8`，仍会编出 class 52 |
| `vuaInit 失败 rc=-2`（运行期） | `.vus` 编译失败或控件表缺失 | 查 `assets/` 是否有 `vua_controls.json` 与全部页面；`vus build vua_test.vus` 定位语法错 |
| 换了页面但 APK 还是旧的 | assets 缓存 | 清理 `examples/vua-android/app/src/main/assets` 后重建（[0] 会重新同步） |
| `apksigner` 未找到 | build-tools 版本不对 | 确认 `$SDK/bt/android-14/apksigner` 存在，或设 `SDK=` 指到含 34.0.0 的 SDK |

---

## 6. 试一把

```bash
cd vus
make
ABI=x86_64 bash examples/vua-android/scripts/build_apk.sh
adb install -r examples/vua-android/dist/VUS.apk
adb shell am start -n com.vus.android/.MainActivity
```

打开后从主页点"**高级能力演示**"，长列表滑动、Markdown 富文本、远程图片
应逐个可用——这三块分别对应 [01-VUA-组件流-从零到高级](01-VUA-组件流-从零到高级.md)
的第 7/8/9 节。