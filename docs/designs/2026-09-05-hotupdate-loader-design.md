> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-05


# VUS 热更加载器 · 设计文档

> 日期：2026-09-05
> 状态：实现中——核心协议已落地（UpdateManager / 单一真源 .so 加载 / 打包目标），待真机验证
> 一句话：宿主 APK 固化为"永驻小加载器 + 文件/网络自举能力"，运行时/业务/界面按「实时」与「下次启动」两档由整包热更更新。

## 1. 背景与目标

VUS 文档 APK 的界面（`.vua`）、逻辑（`.vus` 编译进 `.so`）、运行时（`libvus_app.so`）与文档资源都在 APK 内，改动一次就要重发 APK。目标是：**发版走"更新包"而非重装**，宿主 APK 只需极小化并几乎不动。

| 目标 | 达成标准 |
|---|---|
| 不重装 | 业务/界面/文档改动只下发更新包 |
| 实时生效 | `.vua`/`.dex` 可实时热更（已实测 .dex） |
| 够落后处理 | `.so` 标记"下次启动生效"，失败自动回滚 |
| 自举 | 更新协议由宿主内置的文件/网络能力驱动，不依赖插件 `.so` |

## 2. 分层模型

```
宿主 APK（唯一永驻，尽量小）
├─ 更新协议：拉 manifest → 下载 → sha256 校验 → .new + rename 原子提交 → 回滚
├─ 启动钩子：单一真源 = filesDir（version 0 起，见 §5.4）
├─ 加载器：ExtensionLoader（DEX 实时，已通）+ .so loader（待接，下次启动生效）
├─ 宿主核心（固化，不进插件）：
│   VuaBridge（JNI 桥 + 文件/网络预置能力）、VuaRenderer / ImageLoader、UpdateChecker
└─ Platform：Activity、extractAssets（内置版本 0 的 patch：缺失才写、不覆盖已有）

插件层（整包替换，filesDir 内）
├─ libvus_app.so     运行时 + .vus 业务      （下次启动生效）
├─ 逻辑插件 .dex      VusExtension 实现        （实时，已实测）
├─ .vua + 控件表/词典 + 资源 + 文档          （实时）
```

关键边界（讨论决策，见 §4）：
- **渲染器等底层核心不进插件层**：`VuaRenderer`/`ImageLoader`/`VuaBridge` 固化宿主；
- 功能性子集（如文档 UI 之上需要的东西）由 `.dex` 逻辑插件与 `.vua` 数据层提供 → 实时热更。

## 3. 热更能力表

| 层 | 更新方式 | 生效时机 | 现状 |
|---|---|---|---|
| `.vua` / 资源 / 控件表 / 文档 | 整包替换 filesDir 文件 | **实时**（重新渲染即生效） | 已通（数据驱动渲染） |
| `.dex` 逻辑插件 | 下载到 `filesDir/plugins/`，[ExtensionLoader](file:///vus/examples/vua-android/app/src/main/java/com/vus/android/ExtensionLoader.java) 按文件修改时间重建 ClassLoader | **实时**（同进程替换，已实测） | 已通 |
| `.so`（运行时） | 文件替换 + 版本标记 | **下次启动**（Android 不允许运行时换已映射库） | 待接 |
| 宿主 APK | 随发布 | 重装 | —— |

## 4. 已确认的架构决策

1. **渲染器/桥固化宿主，不进 `.dex`**：`VuaRenderer` 与 `VuaBridge`（JNI）、View 树生命周期、`MainActivity` 重渲染循环深度耦合；界面形态变化由 `.vua` 数据层实时热更覆盖，拆进 dex 收益不抵代价。
2. **不做 ABI 版本冻结治理**："底层只有加载器、能力全在插件层"的整包世界观下，不需要维持"新宿主配老插件"的兼容矩阵。但保留**构建期自检**：`gen_jni_bridge.py --expect` 校验新 `.so` 的 `Java_*` 导出 ⊇ 宿主 Java 声明（否则 JNI 调用 `UnsatisfiedLinkError`），打包更新包时复用，不新增维护成本。
3. **`.so` 只承诺"下次启动生效"**：实时热更仅限数据层（.vua/资源/控件表）与 `.dex`（ExtensionLoader 已这样跑通）。同进程运行时换 JNI 表（档 C）在 ART 上不可行，排除；隔离进程热切（档 B）复杂度来自插件承载四大组件，如无驻留服务需求不引入。
4. **更新协议自举**：协议本体放宿主 Java（`UpdateManager`，纯 `HttpURLConnection` + `ZipInputStream`，不依赖插件 `.so` 甚至不依赖 native），拉清单/下载/校验/替换全在宿主内完成。
5. **主线以 `.vua` + `.dex` 承载业务**（实时），`.so` 仅在动 C 层时更新（重启生效），宿主预置文件/网络只是"让自己能自举"的最小集。

## 5. 更新协议设计

### 5.1 更新包与 manifest

更新包 = zip（文件集合 + `manifest.json`），`manifest.json`：

```json
{
  "版本": "1.0.3",
  "最低宿主版本": 12,                    // APK versionCode，防旧宿主拉新插件（契约错配）
  "文件": {
    "lib/arm64-v8a/libvus_app.so": "sha256:a1b2...",
    "plugins/renderer.dex": "sha256:...",   // 可选，逻辑插件
    "vua_home.vua": "sha256:...",
    "vua_controls.json": "sha256:...",
    "docs/01-使用入门.md": "sha256:...",
    "icons/settings.png": "sha256:..."
  }
}
```

- 版本号**单调递增**；客户端记录已用版本，拒绝版本号 ≤ 当前的包（防降级）。
- `最低宿主版本`：旧宿主不解新包，提示升级 APK。

**路径穿越防护（必须）**：manifest 里的相对路径**不得直接拼接 filesDir** 解包/写入，恶意包（或将来开放第三方源）可用 `../..` 越界写。规则：

1. 解包前对 manifest 每条相对路径做**规范化校验**：`new File(filesDirBase, rel).getCanonicalPath()` 必须以 `filesDirBase.getCanonicalPath()` 加上路径分隔符为前缀（或相等且非目录段），否则拒绝该包；
2. zip 内条目同样逐条校验（经典 zip-slip）：任何条目解压后的目标路径越出 patch 根即拒绝；
3. 拒绝绝对路径、空段、`..` 段；发现任一非法条目 → 整包拒绝，回滚到 last-good。

### 5.2 拉取 → 校验 → 原子提交

```
1. HTTP 拉 manifest（VuaBridge.http.get，宿主内建）
2. 解析版本：不接受 ≤ 当前；检查最低宿主版本
3. 下载整包 zip（http.download → filesDir/patch/下载/）
4. 解包到 filesDir/patch/暂存/（保留相对路径；逐条目路径穿越校验，见 §5.1）
5. 逐文件 sha256 校验；任一失败 → 丢弃暂存目录，保留 last-good，不生效
6. 全部通过 → 逐文件写 .new 后 rename 原子替换到 filesDir 目标路径
7. 写 filesDir/patch/version（版本号标记）
8. 触发生效：.vua/.dex 立即（重新渲染/重载插件）；.so 等待下次启动
```

**last-good 的晋升时机（必须）**：当前版本快照晋升为 last-good **不能在"应用新包"时发生**，否则"应用 → 覆盖 last-good → 重启发现 .so 起不来"就回不去了。正确时序：

- **应用成功后（实时层 .vua/.dex 已生效）→ 不晋升**；
- **下次启动、`.so` 加载验证通过后**才把当前已应用版本快照晋升为 last-good；
- 数据层实时生效与 .so 下次生效的**时间差窗口内**，回滚仍指向旧 last-good，保证任何一步失败都能回到上一可用整体。

任一步失败：清理暂存与 `.new`，下次启动回滚到 `patch/last-good/`（回滚同样先校验再替换）。

### 5.3 目录布局（建议）

```
filesDir/
├─ patch/
│  ├─ version                 # 当前已应用版本号（单调）
│  ├─ last-good/              # 上一版整体快照（回滚用）
│  └─ 暂存/                   # 下载解包校验中间态
├─ plugins/                   # .dex 逻辑插件（ExtensionLoader）
├─ …（assets 释放后的 .vua/资源/文档）
└─ lib.so 等运行时文件
```

### 5.4 启动钩子：单一真源 = filesDir（内置版本即"版本 0 的 patch"）

把"内置版本"统一建模为 **patch 版本 0（由 assets 释放产生）**，filesDir 永远是唯一真源，启动钩子只剩一个分支：

1. 首次启动：`patch/version` 不存在 → 执行"版本 0 的 patch" = `extractAssets`（assets 对 filesDir **缺失才写、不覆盖已有**），写 `patch/version = 0`；
2. 后续启动：读 `patch/version`（无论 0 还是 N ≥ 1）→ 直接以 filesDir 为真源；
3. `.so` 加载同一规则：`filesDir` 下的 `libvus_app.so` 优先，缺失才回退 APK 内 `lib/<abi>/libvus_app.so`。

收益：

- **没有"有没有 patch"之分**：任何文件在 filesDir 可见即真源，版本号只用于防降级与回滚触发；
- **patch 产物永不被 assets 覆盖**：`extractAssets` 缺失才写（不覆盖已有文件），升级 APK 不会把已热更文件打回内置版本（如需强制重置可清 `patch/` 目录）；
- **回滚 = 用 last-good 快照替换 filesDir 后再走起钩子**，逻辑与首装一致。

## 6. 预置能力清单（宿主内建，自举最小集）

| 能力 | 实现 | 用途 |
|---|---|---|
| `http.get/post/request/upload/download` | `VuaBridge.doHttp/doUpload` | 拉 manifest、下载更新包 |
| `file.read/write/append/exists/delete/list/isdir` | `VuaBridge.doFile` | 校验、写 .new、写 version、回滚 |
| `ExtensionLoader`（DexClassLoader + sha256 + 时间戳重建） | 宿主 `ExtensionLoader` | .dex 实时热更 |
| `extractAssets`（全量释放 assets） | `MainActivity.copyTree` | 内置版本的落地基础 |

## 7. 打包与发布（待做）

`build_apk.sh` 已能产出全部更新源（`.so` 多 ABI + dex + assets）。新增目标 **`打包更新包`**：

```
打包更新包:
1. 收集 libvus_app.so、plugins/*.dex、assets 下 .vua/.json/资源/文档
2. 生成 manifest.json（版本 + 逐文件 sha256 + 最低宿主版本）
3. zip 整包 → 更新包.vus​pk（或 .zip），附整体 sha256
```

同一份包既发布到更新服务，也可由桌面 CLI 用于"目录替换 + 重启"调试（协议两端共用 manifest 格式）。

## 8. 安全与信任

- 每个文件 sha256 全部校验通过才提交；暂存与 `.new` 只允许校验通过后 rename。
- 版本号单调 + `最低宿主版本`，防降级与契约错配。
- 宿主加载器代码量小、几 KB，校验/替换/回滚路径不依赖插件，保持稳定。
- 自分发场景 HTTPS + sha256 已够；如后续引入强校验再叠加签名。

## 9. 边界与限制

- **`.so` 不能实时热更**：Android 不允许运行时卸载已映射库；必须"标记 → 下次启动生效"。
- **宿主核心（VuaRenderer/VuaBridge/加载器）不随更新包变**：改动它们 = 发版 APK；业务向 `.vua`/`.dex` 收敛后此类需求应很少。
- **`.vus` 业务编译进 `.so`**：改行 `.vus` 在 APK 内 = 换 `.so` 包（下次启动生效）；实时逻辑热更走 `.dex`（`VusExtension`）。
- 隔离进程热切（新进程载新 `.so`、切换、杀旧进程）仅在需要驻留服务时评估，本期不做。

## 10. 现有机制对照与待实现清单

| 项 | 现状 | 待做 |
|---|---|---|
| .dex 实时加载 + 校验 | ✅ ExtensionLoader（lastModified 重建 ClassLoader、sha256 强制） | —— |
| .vua/资源数据层 | ✅ 数据驱动渲染 + 内置版本 0 释放（缺失才写，extractAssets） | —— |
| 文件/网络桥 | ✅ VuaBridge | —— |
| `.so` load（单一真源） | ✅ VuaBridge.ensureNative：filesDir/lib 优先，缺失回退 APK 内；加载成功触发晋升 | 真机验证 |
| manifest 拉取/解析 | ✅ UpdateManager.applyUpdate（HttpURLConnection + org.json） | 真机验证 |
| 原子提交 + 回滚 | ✅ UpdateManager：`.new`+rename、last-good 晋升（onSoLoaded）、needs_rollback 回滚、路径穿越校验 | 真机验证 |
| 打包更新包 | ✅ scripts/pack_update.sh（manifest + zip + sha256，已产出验证） | —— |

## 11. 参考

- 加载器现状：`examples/vua-android/app/src/main/java/com/vus/android/ExtensionLoader.java`
- 宿主桥：`examples/vua-android/app/src/main/java/com/vus/android/VuaBridge.java`
- 资产释放：`MainActivity.copyTree`（全量释放 assets → filesDir）
- 构建链：`examples/vua-android/scripts/build_apk.sh` / `plugins/build_plugin.sh`
- 插件契约：`VusExtension.java`、`plugins/sample/java/com/vus/plugins/SamplePlugin.java`