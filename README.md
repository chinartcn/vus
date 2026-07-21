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

## 一键安装

```bash
# 方法一：一键安装脚本（推荐）
curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | bash

# 方法二：手动克隆并编译
git clone --depth 1 https://gitee.com/rtccn_mc/vus.git ~/.vus
cd ~/.vus && make
ln -s ~/.vus/vus ~/.local/bin/vus
```

安装后，重新打开终端或执行 `source ~/.bashrc`，然后运行：

```bash
vus init               # 初始化项目（选择语法风格）
vus run main.vus       # 编译并运行
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
| `vus run` | 构建 + 运行生成的二进制文件 |
| `vus run --debug` | 调试模式运行（含栈追踪和调试输出） |
| `vus test` | 自动执行 `测试/` 目录下全部测试用例 |
| `vus lang list` | 列出已安装语言插件（.vulage） |
| `vus vux list` | 列出已安装功能插件（.vux） |
| `vus vusx list` | 列出项目中的 vusx 依赖 |

## 版本规划

| 版本 | 范围 | 状态 |
|------|------|------|
| **v0.1 PoC** | 函数风格 + 打印/四则运算 + 基础运行时 | ✅ 完成 |
| **v0.1 正式版** | 双语法 + 流程控制 + 函数/闭包 + 四层插件体系 | ✅ 完成 |
| **v0.2** | 错误信息中文化 + 精准行号定位 + 调试模式 + 栈追踪 + 性能优化 | ✅ 完成 |
| **v1.0** | 泛型、结构体、多线程、异步、自举 | 🚀 未来 |

## 开源协议

[MIT](LICENSE) © 2026 VUS Language Contributors