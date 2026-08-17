<p align="center">
  <img src="vus-logo.png" alt="VUS Logo" width="256">
</p>

# VUS 编程语言

> **中西合璧，天下无敌**  
> 让不懂英文的人也能写代码，同时保留对底层硬件的完全控制权

VUS 是面向 Linux、Android Termux、嵌入式 ARM 设备的中文友好多范式**编译型**编程语言。填补 C 与 Python 之间的空白，编译到 C → GCC/Clang → 原生可执行文件。

当前版本：**v1.0-beta**

## 📖 文档

- **[教程](docs/TUTORIAL.md)** — 从零开始学会 VUS（安装、语言基础、运算符、流程控制、函数、线程协程、插件运行时函数）
- **[语言参考](docs/LANGUAGE_REFERENCE.md)** — 已实现的关键字 / 运算符 / 类型 / 语句 / 函数清单，并明确区分「已实现 vs 尚未实现」
- **[API 参考](docs/API_REFERENCE.md)** — C ABI 与四层插件体系、运行时库的公开接口（基于真实头文件）
- **[架构](docs/ARCHITECTURE.md)** — 编译流水线、词法 / 语法 / AST / 代码生成、运行时、APK 打包等实现机制
- **[生态](docs/ECOSYSTEM.md)** — 生态组件、四层插件体系、CLI、目录结构、依赖关系
- **[状态](docs/STATUS.md)** — 功能清单、测试状态、已知 Bug 与限制

## 特性

- **中文友好**：核心语法支持函数风格，英文关键字与中文别名可混用；易语言风格（`.功能`/`.结束`）通过 `.vulage` 语言插件在编译前预处理实现
- **编译到 C**：VUS → C → 原生可执行文件，无解释器依赖
- **类型注解**：`变量: 类型`、参数注解、泛型参数 `名<T>()` 均被解析记录
- **自动内存管理**：引用计数（`vus_ref`/`vus_unref`），无需手动管理
- **结构体**：`结构`/`struct` 定义字段 + `类型名(...)` 构造 + `.` 链式成员访问
- **线程与协程**：`线程`/`等待线程`（POSIX 线程）、`协程`/`恢复`/`让出`（支持 Termux/Android 的轻量协程）
- **异常处理**：`尝试`/`捕获`/`抛出`（错误码链，非 try-setjmp）
- **APK 打包**：`vus build --apk` 生成 Android 项目（含 JNI 桥接、Android.mk、Manifest）
- **插件运行时函数**（已实现）：
  - **TUI**：`tui_清屏` `tui_设置颜色` `tui_定位` `tui_进度条` `tui_重置`（ANSI 转义，无外部依赖）
  - **网络**：`网络_GET` `网络_POST` `网络_下载`（需链接 libcurl / `VUS_HAVE_CURL`）
  - **文件**：`文件_读取` `文件_写入` `文件_追加` `文件_存在` `文件_删除` `文件_列表`
  - **日期时间**：`日期_现在` `日期_时间戳` `日期_格式化` `日期_解析` `日期_年/月/日/时/分/秒`
- **分级日志**：`日志_调试/信息/警告/错误/级别`（集成 EasyLogger）
- **四层插件体系**：`.vus`（源码）→ `.vusx`（VUS 插件，编译期）→ `.vux`（功能插件，运行期）→ `.vulage`（语言插件，编译前）
- **错误信息中文化**：编译报错含文件、行号、列号；`vus run --debug` 提供调用栈追踪

> ⚠️ **如实说明（请勿按旧文档误用）**：
> - 异步 `等待` 仅为已定义 token，**解析器尚未实现**。
> - **类型注解不执行强类型检查**、**泛型目前为语法形态**（类型实参仅作为注释注入，无真实实例化），与设计文档的「强类型」存在差距。
> - 旧式标准库名（`断言`/`长度`/`拼接`/`分割`/`替换`/`取子串`/`遍历列表`/字典旧名等）已实现的对应版本为上面的 `文件_*`/`日期_*`/`网络_*`/`tui_*`，**其余旧名未实现**，调用会导致链接失败。
> - **GUI（`图形_*`/guilite/guilibridge 等）尚未完成，不属于当前可用能力。**

## 生态与示例仓库

- **[官方示例库](https://gitee.com/rtccn_mc/vus-example.git)** — VUS 官方示例程序合集，便于对照学习
- **[官方插件包仓库](https://gitee.com/rtccn_mc/official-vus.vux-package.git)** — 官方 `.vux` 功能插件包的发布来源，可通过 `vus vux install <名称>` 安装

## 社区

| 渠道 | 用途 |
|------|------|
| **[百度贴吧 · VUS语言吧](https://tieba.baidu.com/wxf/28312232?kw=vus%E8%AF%AD%E8%A8%80&fr=sharewise)** | 讨论交流、问题反馈、社区活动 |
| **Gitee Issues** | Bug 报告、功能请求 |
| **邮箱** | 私下联系（rtcn_0523@qq.com） |

## 一键安装

```bash
# 方法一：一键安装脚本（推荐，自动检测架构）
curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | bash

# 方法二：手动克隆并编译（通用，支持所有架构）
git clone --depth 1 https://gitee.com/rtccn_mc/vus.git ~/.vus
cd ~/.vus && make
ln -s ~/.vus/vus ~/.local/bin/vus
```

安装后，重新打开终端或执行 `source ~/.bashrc`，然后运行：

```bash
vus init               # 初始化项目
vus run main.vus       # 编译并运行
```

### ARM 设备安装（树莓派、Orange Pi、Termux）

VUS 支持 ARM64（aarch64）和 ARM32（armhf）架构。安装脚本会自动检测架构并尝试下载预编译包：

```bash
# ARM64 / ARM32 通用安装（自动检测）
curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | bash

# 如需指定安装目录
curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | VUS_HOME=/data/local/tmp/vus sh

# 树莓派手动编译（如果预编译包不可用）
sudo apt install git gcc make
git clone --depth 1 https://gitee.com/rtccn_mc/vus.git ~/.vus
cd ~/.vus && make
ln -s ~/.vus/vus ~/.local/bin/vus
```

### 构建预编译包（开发者）

```bash
# 需要安装交叉编译工具链做交叉编译
sudo apt install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf

# 构建发布包（scripts/build_release.sh）
bash scripts/build_release.sh
```

## 一键更新

```bash
# 方法一：重新运行安装脚本
curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | bash

# 方法二：手动更新
cd ~/.vus && git pull --ff-only
```

## 快速入门

### 函数风格（推荐给熟悉 Python 的用户）

```
#// main.vus
定义 问候(名字):
    打印("你好，" .. 名字 .. "！")

问候("世界")
```

### 易语言风格（零基础用户，需配置并加载“易语言”语言插件）

```
#// main.vus
.功能 问候(名字)
    .打印("你好，" .. 名字 .. "！")
.结束

.问候("世界")
```

### 编译运行

```bash
vus build --c-only main.vus    # 仅编译为 C 代码（输出到 构建/ 目录）
vus build --exe   main.vus    # 编译为可执行文件
vus run           main.vus    # 编译并运行
```

## 项目结构

```
我的项目/
├── vus.json          # 项目配置（风格、语言插件、vusx依赖、构建目录、优化等）
├── main.vus          # 主程序
├── libs/             # 库模块
│   └── 工具.vus
├── 测试/             # 测试用例
│   └── test_工具.vus
├── 资源/             # 资源文件
│   └── config.json
├── 构建/             # 编译输出（自动生成）
│   └── main.c
└── .vus_cache/       # 编译缓存（自动生成）
```

## 命令速查

| 命令 | 作用 |
|------|------|
| `vus init [--force]` | 新项目交互式初始化 / 强制重建配置 |
| `vus build --c-only <文件>` | 仅输出 C 文件 |
| `vus build --exe <文件>` | 编译 C 源码并调用 GCC 生成可执行程序 |
| `vus build --apk <文件>` | 编译为 Android APK 项目（可选 `--ndk-path`/`--app-name`/`--output`）|
| `vus run <文件>` | 构建 + 运行生成的二进制文件 |
| `vus run --debug <文件>` | 调试模式运行（含栈追踪和调试输出）|
| `vus test` | 运行测试用例 |
| `vus lang list / load / info` | 语言插件管理（.vulage）|
| `vus vux install / build / info / list / run` | 功能插件管理（.vux）|
| `vus vusx list / info / build` | VUS 插件管理（.vusx）|
| `vus update` | 自动更新编译器 |
| `vus --version / -v` | 显示版本信息 |
| `vus --help / -h` | 显示帮助信息 |

## 版本规划

| 版本 | 范围 | 状态 |
|------|------|------|
| **v0.1** | 基础语言 + 插件体系 | ✅ 完成 |
| **v0.2** | 调试体验优化 + 预编译包 + 安装脚本 | ✅ 完成 |
| **v1.0-alpha** | 泛型 + 结构体 + 多线程 + 异步 + apk 打包 | ✅ 完成 |
| **v1.0-beta** | 语言核心稳定 + apk 修复优化 + tui/网络/文件/日期 插件运行时函数 | ✅ 完成（当前版本）|
| **v1.0** | 正式版 + 加密/数据库等 | 🚀 未来 |
| **后续 v1.1+** | 图形插件、性能优化、包管理器、文档完善、代码打磨等 | 🚀 未来 |

## 开源协议

[MIT](LICENSE) © 2026 VUS Language Contributors