<p align="center">
  <img src="vus-logo.png" alt="VUS Logo" width="256">
</p>

# VUS 编程语言

> **中西合璧，天下无敌**  
> 让不懂英文的人也能写代码，同时保留对底层硬件的完全控制权

VUS 是面向 Linux、Android Termux、嵌入式 ARM 设备的中文友好多范式编译型强类型编程语言。填补 C 与 Python 之间的空白，编译到 C → GCC/Clang → ARM/x86 原生可执行文件。

📖 **完整教程**: [docs/TUTORIAL.md](docs/TUTORIAL.md) — 从零开始学会 VUS

## 特性

- **中文友好**：支持函数风格（中英混写）和易语言风格（通过 .vulage 语言插件）
- **编译到 C**：VUS → C → 原生可执行文件，无解释器依赖
- **强类型系统**：自动类型推断 + 显式注解，编译时类型检查
- **自动内存管理**：引用计数 + 循环检测 GC，无需手动管理
- **四层插件体系**：`.vus`(源码) → `.vusx`(VUS插件) → `.vux`(功能插件) → `.vulage`(语法插件)
- **错误信息中文化**：编译器报错信息为中文，包含文件名、行号、列号
- **调试模式**：`vus run --debug` 支持运行时栈追踪和调试输出
- **Python 桥接**（可选）：内联 Python 代码，无缝调用 Python 生态

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
# 需要安装交叉编译工具链
sudo apt install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf

# 构建所有架构
bash scripts/build_release.sh v0.2

# 输出在 release/ 目录
ls -lh release/*.tar.gz
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

### 易语言风格（零基础用户）

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
├── vus.json          # 项目配置（风格、依赖、编译选项）
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
| `vus init` | 新项目交互式初始化 |
| `vus build --c-only` | 仅输出 C 文件 |
| `vus build --exe` | 编译 C 源码并调用 GCC 生成可执行程序 |
| `vus build --apk` | 编译为 Android APK 项目（可选 --ndk-path） |
| `vus run` | 构建 + 运行生成的二进制文件 |
| `vus run --debug` | 调试模式运行（含栈追踪和调试输出） |
| `vus test` | 自动执行 `测试/` 目录下全部测试用例 |
| `vus lang list` | 列出已安装语言插件（.vulage） |
| `vus vux list` | 列出已安装功能插件（.vux） |
| `vus vusx list` | 列出项目中的 vusx 依赖 |

## 版本规划

| 版本 | 范围 | 状态 |
|------|------|------|
| **v0.1** | 基础语言 + 插件体系 | ✅ 完成 |
| **v0.2** | 调试体验优化 + 预编译包 + 安装脚本 | ✅ 完成 |
| **v1.0-alpha** | 泛型 + 结构体 + 多线程 + 异步 + apk 打包 | ✅ 完成 |
| **v1.0-beta** | 语言核心稳定 + apk 修复优化 + tui/网络/文件/日期 插件 | ✅ 完成 |
| **v1.0** | 正式版 + 加密/数据库/易语言风格 | 🚀 未来 |
| **v1.1 - v1.9** | vus2d/vus3d 图形插件开发 + 测试 | 🚀 未来 |
| **v2.0 - v2.5** | 基准测试 + 性能优化 + 错误恢复 + 代码审查 | 🚀 未来 |
| **v3.0 - v3.1** | 用户手册 + 开发者文档 + API 参考 | 🚀 未来 |
| **v4.0 - v4.9** | 语法/类型/运行时/插件/C ABI 规范正式化 | 🚀 未来 |
| **v5.0 - v5.5** | APK 打包工具开发（apk.vux 插件） | 🚀 未来 |
| **v5.6 - v5.7** | 正式推广 + 示例项目 | 🚀 未来 |
| **v6.0 - v6.x** | 收集用户反馈，整理需求清单 | 🚀 未来 |
| **v7.0** | 包管理器仓库（`vus install` 生态） | 🚀 未来 |
| **v8.0** | 编译器自举（VUS 编译自身） | 🚀 未来 |
| **v9.0 - v9.5** | 代码打磨（审查/重构/内存安全/边界测试） | 🚀 未来 |
| **v10.0** | IDE + 可视化编辑 | 🚀 未来 |
| **v11.0** | 积木化编辑（拖拽式积木编程） | 🚀 未来 |

```
v0.1 → v0.2 → v1.0-alpha → v1.0-beta → v1.0 → ... → v1.9 → v2.0 → ... → v5.5 → v5.6 → v6.x → v7.0 → v8.0 → v9.5 → v10.0 → v11.0
```

## 开源协议

[MIT](LICENSE) © 2026 VUS Language Contributors