# VUS 语言完整教程

> 从零开始，学会用 VUS 编写中文编程语言程序。
> 本教程面向零基础用户，也适合有编程经验的开发者快速上手。

---

## 目录

1. [安装与第一个程序](#1-安装与第一个程序)
2. [语言基础](#2-语言基础)
3. [运算符](#3-运算符)
4. [流程控制](#4-流程控制)
5. [函数](#5-函数)
6. [综合示例](#6-综合示例)
7. [CLI 命令](#7-cli-命令)
8. [插件系统](#8-插件系统)
9. [C ABI 接口](#9-c-abi-接口)
10. [故障排除](#10-故障排除)

---

## 1. 安装与第一个程序

### 1.1 一键安装

```bash
curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | bash
```

安装脚本会自动：
- 检查依赖（`git`、`gcc`、`make`）
- 克隆仓库并编译
- 创建 `~/.local/bin/vus` 符号链接
- 将 `~/.local/bin` 加入 PATH
- 可选运行 20 项一键功能测试

安装后执行 `source ~/.bashrc`（或重新打开终端），然后：

```bash
vus --version
# 输出: VUS 编译器 v0.1
#       ABI 版本: 1.0.0
```

### 1.2 第一个程序

创建 `hello.vus`：

```
#// 第一个 VUS 程序
打印("Hello, World!\n")
```

编译并运行：

```bash
vus run hello.vus
# 输出: Hello, World!
```

### 1.3 编译为可执行文件

```bash
vus build --c-only hello.vus    # 仅编译为 C 代码
vus build --exe   hello.vus    # 编译为可执行文件
./构建/hello                    # 直接运行
```

### 1.4 工作原理

VUS 编译流水线：

```
VUS 源码 (.vus)
    ↓ 词法分析 (Lexer)
Token 流
    ↓ 语法分析 (Parser)
抽象语法树 (AST)
    ↓ 代码生成 (Generator)
C 源码 (.c)
    ↓ GCC 编译 (Compiler)
可执行文件
```

---

## 2. 语言基础

### 2.1 注释

```
#// 这是多行注释
#// 可以写多行

# 这是行尾注释
打印("Hello\n")  # 行尾注释
```

### 2.2 变量

VUS 是动态类型语言，所有值都是字符串。变量无需声明类型，直接赋值即可：

```
姓名 = "张三"
年龄 = 25
身高 = 175.5
```

变量名支持中文、英文、数字和下划线，但必须以字母或中文开头：

```
合法的名字 = "OK"
不合法的123 = "NO"    # 错误：不能以数字开头
_value1 = "test"      # 合法
```

### 2.3 变量赋值

变量可以多次赋值，类型自动跟随：

```
x = 10
x = x + 1         # x 现在是 11
x = "hello"       # x 现在是字符串
```

### 2.4 内置函数

| 函数 | 说明 | 示例 |
|------|------|------|
| `打印(值)` | 输出文本 | `打印("Hello\n")` |
| `输入()` | 从键盘读取一行 | `名字 = 输入()` |
| `转数字(值)` | 字符串转整数 | `n = 转数字("123")` |
| `转文本(值)` | 数字转字符串 | `s = 转文本(456)` |

---

## 3. 运算符

### 3.1 算术运算符

| 运算符 | 说明 | 示例 | 结果 |
|--------|------|------|------|
| `+` | 加法（或字符串拼接） | `10 + 20` | `30` |
| `-` | 减法 | `20 - 5` | `15` |
| `*` | 乘法 | `6 * 7` | `42` |
| `/` | 整数除法 | `10 / 3` | `3` |
| `%` | 取余 | `10 % 3` | `1` |

`+` 运算符智能识别类型：
- 两个操作数都是数字 → 算术加法
- 任一操作数不是数字 → 字符串拼接

```
一 = 1
二 = 2
打印(一 + 二)          # 输出: 3（算术加法）

问候 = "你好"
名字 = "世界"
打印(问候 + 名字)     # 输出: 你好世界（字符串拼接）
```

### 3.2 比较运算符

| 运算符 | 说明 | 示例 |
|--------|------|------|
| `<` | 小于 | `如果 a < b:` |
| `<=` | 小于等于 | `如果 a <= 10:` |
| `>` | 大于 | `如果 b > a:` |
| `>=` | 大于等于 | `如果 b >= 20:` |
| `==` | 等于 | `如果 a == 10:` |
| `!=` | 不等于 | `如果 a != b:` |

### 3.3 运算符优先级（从低到高）

1. `..`（字符串拼接）
2. `+` `-`（加减）
3. `*` `/` `%`（乘除）
4. 一元运算符
5. 括号 `()`

```
结果 = 1 + 2 * 3     # 结果 = 7（先乘后加）
结果 = (1 + 2) * 3   # 结果 = 9（括号优先）
```

---

## 4. 流程控制

### 4.1 条件分支：`如果`/`否则如果`/`否则`

```
分数 = 85

如果 分数 >= 90:
    打印("优秀\n")
否则如果 分数 >= 80:
    打印("良好\n")
否则如果 分数 >= 60:
    打印("及格\n")
否则:
    打印("不及格\n")
```

注意：`否则如果` 是连写的关键字，中间不能有空格。

### 4.2 范围循环：`循环...从...到...`

```
循环 i 从 1 到 5:
    打印(i)
# 输出: 1 2 3 4 5
```

循环变量 `i` 从起始值到结束值（**包含结束值**），步长为 1。

### 4.3 当循环：`当循环`

```
x = 1
当循环 x <= 5:
    打印(x)
    x = x + 1
# 输出: 1 2 3 4 5
```

### 4.4 嵌套循环

```
循环 i 从 1 到 9:
    循环 j 从 1 到 i:
        打印(i * j)
        打印(" ")
    打印("\n")
# 输出: 完整的 9x9 乘法表
```

---

## 5. 函数

### 5.1 定义和调用

```
定义 问候(名字):
    打印("你好，" + 名字 + "！\n")

问候("张三")
# 输出: 你好，张三！
```

### 5.2 返回值

```
定义 加法(a, b):
    返回 a + b

结果 = 加法(10, 20)
打印(结果)
# 输出: 30
```

### 5.3 递归函数

```
定义 阶乘(n):
    如果 n <= 1:
        返回 1
    返回 n * 阶乘(n - 1)

打印(阶乘(5))
# 输出: 120
```

```
定义 斐波那契(n):
    如果 n <= 1:
        返回 n
    返回 斐波那契(n - 1) + 斐波那契(n - 2)

循环 i 从 0 到 10:
    打印(斐波那契(i))
# 输出: 0 1 1 2 3 5 8 13 21 34 55
```

### 5.4 函数返回布尔值

由于 `如果` 条件检查字符串 `"true"`，函数返回布尔值时应使用比较表达式：

```
定义 是偶数(n):
    返回 n % 2 == 0    # 返回 "true" 或 "false"

如果 是偶数(10):
    打印("是偶数\n")
```

---

## 6. 综合示例

### 6.1 判断质数

```
定义 是质数(n):
    如果 n < 2:
        返回 0 == 1    # 返回 "false"
    循环 i 从 2 到 n - 1:
        如果 n % i == 0:
            返回 0 == 1
    返回 0 == 0        # 返回 "true"

打印("2 到 50 之间的质数：\n")
循环 i 从 2 到 50:
    如果 是质数(i):
        打印(i)
        打印(" ")
# 输出: 2 3 5 7 11 13 17 19 23 29 31 37 41 43 47
```

### 6.2 累加求和

```
和 = 0
循环 i 从 1 到 100:
    和 = 和 + i
打印(和)
# 输出: 5050
```

### 6.3 完整演示

请参考 `tests/test_demo.vus`，这是一个包含 10 个功能模块的 241 行综合演示脚本：

```bash
vus run tests/test_demo.vus
```

输出预览：

```
╔══════════════════════════════════════╗
║       VUS 语言 v0.1 综合功能演示      ║
╚══════════════════════════════════════╝

┌── 1. 变量与赋值 ──────────────────┐
│ 姓名: 小明
│ 年龄: 25
│ 身高: 175
...
```

---

## 7. CLI 命令

### 7.1 命令速查

| 命令 | 说明 |
|------|------|
| `vus run <文件>` | 编译并运行 VUS 程序 |
| `vus build --c-only <文件>` | 仅编译为 C 代码 |
| `vus build --exe <文件>` | 编译为可执行文件 |
| `vus init` | 初始化项目 |
| `vus --version` / `-v` | 显示版本信息 |
| `vus --help` / `-h` | 显示帮助信息 |

### 7.2 插件管理命令

| 命令 | 说明 |
|------|------|
| `vus lang list` | 列出已安装的语言插件（.vulage） |
| `vus lang load <文件>` | 加载语言插件共享库 |
| `vus lang info <名称>` | 查看语言插件详细信息 |
| `vus vux install <源>` | 安装 .vux 插件 |
| `vus vux build [目录]` | 打包 .vux 插件 |
| `vus vux info <插件>` | 查看插件信息 |
| `vus vux list` | 列出已安装插件 |
| `vus vux run <插件> [输入]` | 运行插件 |
| `vus vusx list` | 列出项目中的 vusx 依赖 |
| `vus vusx info <路径>` | 查看 vusx 插件信息 |
| `vus vusx build <路径>` | 编译 vusx 插件 |

### 7.3 配置文件 `vus.json`

项目根目录的 `vus.json` 控制编译选项：

```json
{
    "风格": "函数",
    "语言插件": "",
    "vusx依赖": [],
    "运行时目录": "rt",
    "构建目录": "构建",
    "优化": "速度"
}
```

- `风格`：`"函数"`（中英混写）或 `"易语言"`（需安装易语言语言插件）
- `语言插件`：语言插件名称（如 `"易语言"`），空表示使用核心语法
- `vusx依赖`：`.vusx` 插件目录路径列表
- `运行时目录`：运行时库路径
- `构建目录`：编译输出目录
- `优化`：`"速度"` 或 `"大小"`

---

## 8. 插件系统

### 8.1 .vux 插件格式

`.vux` 文件是 zip 压缩包，结构如下：

```
插件名.vux
├── vux.json          # 元数据（必需）
├── __init__.py       # 插件入口（必需，继承 VuxPlugin）
├── vux依赖.txt       # 其他 .vux 包依赖（可选）
├── vuxpy依赖.txt     # Python 包依赖（可选）
└── 资源/             # 静态资源（可选）
    ├── 图标.png
    └── 样式.css
```

### 8.2 vux.json 元数据

```json
{
    "名称": "我的插件",
    "版本": "1.0.0",
    "作者": "VUS Team",
    "描述": "插件功能描述",
    "入口": "__init__.py",
    "最低VUS版本": ">=0.1.0",
    "依赖": {
        "db.vux": ">=1.2.0"
    },
    "Python依赖": {
        "requests": ">=2.28.0"
    }
}
```

### 8.3 编写 Python 插件

```python
# __init__.py
from vux_plugin_entry import VuxPlugin, VuxPluginAPI

class 我的插件(VuxPlugin):
    def init(self, api):
        """初始化插件。返回 0 成功。"""
        print(f"插件初始化，ABI 版本: {api.version}")
        return 0

    def run(self, api, input_data):
        """执行插件功能。返回 (code, output)。"""
        # 调用 VUS 编译器编译代码
        result = api.compile_string('打印("Hello from plugin!\\n")')
        return 0, f"处理完成: {input_data}"

    def cleanup(self, api):
        """清理资源。"""
        pass
```

### 8.4 打包和安装

```bash
# 打包为 .vux
vus vux build ./my-plugin

# 安装
vus vux install 我的插件-1.0.0.vux

# 查看已安装插件
vus vux list

# 运行插件
vus vux run 我的插件 "测试输入"
```

---

### 8.5 .vulage 语言插件（语法风格）

`.vulage` 是**语言插件**，在编译前预处理源码，将非标准语法转换为标准 VUS 函数风格。适合需要自定义语法风格的用户。

#### 工作原理

```
VUS 源码（易语言风格等）
    ↓
[语言插件预处理] — 在词法分析之前转换源码
    ↓
标准 VUS 函数风格源码
    ↓
[正常编译流水线] → 可执行文件
```

#### 目录结构

```
plugins/lang/易语言/
├── vux.json          # 插件元数据
└── __init__.py       # 预处理脚本（Python）
```

#### vux.json 元数据

```json
{
    "名称": "易语言",
    "版本": "1.0.0",
    "类型": "vulage",
    "描述": "易语言风格的 VUS 语法插件",
    "入口": "__init__.py"
}
```

#### 编写语言插件

```python
# plugins/lang/易语言/__init__.py
class 易语言:
    def preprocess(self, source):
        """将易语言风格转换为标准 VUS 函数风格。"""
        lines = source.split('\n')
        result = []
        for line in lines:
            stripped = line.strip()
            if stripped.startswith('.'):
                # 转换 .关键字 → 关键字
                line = line.replace('.功能 ', '定义 ', 1)
                line = line.replace('.如果 ', '如果 ', 1)
                line = line.replace('.否则', '否则')
                line = line.replace('.返回 ', '返回 ', 1)
                line = line.replace('.结束', '')
                line = line.replace('.打印(', '打印(')
                line = line.replace('.循环 ', '当循环 ', 1)
            result.append(line)
        return '\n'.join(result)
```

#### 使用语言插件

```bash
# 在 vus.json 中配置
{
    "语言插件": "易语言"
}

# 编译时自动加载
vus run main.vus
```

---

### 8.6 .vusx 插件（VUS 编写）

`.vusx` 是**用 VUS 自身编写的功能插件**，在编译时自动编译并链接到主程序。这是 VUS 的"自举"第一步——用 VUS 扩展 VUS。

#### 工作原理

```
读取 vus.json → 解析 vusx 依赖
    ↓
编译 .vusx 插件（VUS → C → .o）
    ↓
编译主程序（VUS → C）
    ↓
GCC 链接：主程序.o + .vusx 插件.o → 可执行文件
```

#### 目录结构

```
my_utils.vusx/
├── vusx.json          # 插件元数据（必需）
└── main.vus           # VUS 源码（必需）
```

#### vusx.json 格式

```json
{
    "名称": "my_utils",
    "版本": "1.0.0",
    "入口": "main.vus",
    "导出": ["问候", "计算"]
}
```

#### 编写 vusx 插件

```vus
#// my_utils.vusx/main.vus
定义 问候(名字):
    打印("你好，" + 名字 + "！\n")

定义 计算(a, b):
    返回 a + b
```

#### 在项目中引用

在 `vus.json` 中添加：

```json
{
    "vusx依赖": ["my_utils.vusx"]
}
```

然后在主程序中使用：

```vus
#// main.vus
问候("世界")
结果 = 计算(10, 20)
打印(结果)
```

#### 管理命令

```bash
# 列出项目中的 vusx 依赖
vus vusx list

# 查看 vusx 插件信息
vus vusx info my_utils.vusx

# 单独编译 vusx 插件
vus vusx build my_utils.vusx
```

---

### 8.7 四层插件体系对比

| 类型 | 扩展名 | 编写语言 | 加载时机 | 作用 |
|------|--------|----------|----------|------|
| **源码** | `.vus` | VUS | 编译时 | 主程序源码 |
| **VUS 插件** | `.vusx` | VUS | 编译时（自动编译+链接） | 用 VUS 扩展 VUS |
| **功能插件** | `.vux` | Python/C | 运行时 | 扩展编译器功能 |
| **语言插件** | `.vulage` | Python/C | 编译前预处理 | 自定义语法风格 |

---

## 9. C ABI 接口

VUS 编译器提供稳定的 C ABI，供外部程序（C/C++、Python、Ruby 等）嵌入调用。

### 9.1 头文件

```c
#include <vus/vus_abi.h>
```

### 9.2 函数说明

| 函数 | 说明 |
|------|------|
| `vus_abi_version()` | 返回 ABI 版本号 |
| `vus_compile_file(path, config)` | 编译 .vus 文件 → C 代码 |
| `vus_compile_string(source, config)` | 从源码字符串编译 → C 代码 |
| `vus_compile_string_to_exe(source, config)` | 编译并链接 → 可执行文件 |
| `vus_eval(code, config, output)` | 求值表达式，捕获 stdout 输出 |

### 9.3 C 语言调用示例

```c
#include <vus/vus_abi.h>
#include <stdio.h>
#include <string.h>

int main() {
    VusConfig config;
    memset(&config, 0, sizeof(config));
    strcpy(config.style, "函数");
    strcpy(config.project_dir, ".");

    // 编译文件
    VusResult res = vus_compile_file("hello.vus", &config);
    if (res.success) {
        printf("编译成功: %s\n", res.c_output_path);
    } else {
        printf("编译失败: %s\n", res.error_msg);
    }

    // 求值表达式
    char output[4096];
    res = vus_eval("1 + 2 * 3", &config, output);
    if (res.success) {
        printf("求值结果: %s\n", output);  // 输出: 7
    }

    return 0;
}
```

### 9.4 Python 调用示例

```python
import ctypes
import subprocess

# 通过 CLI 调用
result = subprocess.run(
    ["./vus", "run", "hello.vus"],
    capture_output=True, text=True
)
print(result.stdout)

# 通过 C ABI 调用（需要 libvus.so）
lib = ctypes.CDLL("./vus")
lib.vus_abi_version.restype = ctypes.c_int
print(f"ABI 版本: {lib.vus_abi_version()}")
```

### 9.5 VusResult 结构体

```c
typedef struct {
    int   success;              // 1=成功, 0=失败
    char  c_output_path[1024];  // 生成的 C 文件路径
    char  exe_output_path[1024];// 可执行文件路径
    char  error_msg[512];       // 错误消息
} VusResult;
```

### 9.6 VusConfig 结构体

```c
typedef struct {
    char style[32];        // 编码风格："函数" / "易语言"
    char project_dir[1024];// 项目目录
    char rt_dir[1024];     // 运行时库目录
    char build_dir[1024];  // 构建输出目录
    char optimization[32]; // 优化级别
    // ... 其他配置字段
} VusConfig;
```

---

## 10. 故障排除

### 10.1 常见问题

| 问题 | 原因 | 解决 |
|------|------|------|
| `vus: command not found` | PATH 未设置 | 执行 `source ~/.bashrc` 或重新打开终端 |
| `未找到 gcc` | 缺少编译工具 | `sudo apt install gcc make` (Ubuntu/Debian) |
| `期望 缩进，但遇到 ...` | 缩进不一致 | 检查是否混用了空格和 Tab，统一使用 4 空格 |
| 编译后程序崩溃 | 运行时库路径错误 | 确保 `rt/` 目录在项目目录中 |
| 中文显示乱码 | 终端编码问题 | 确保终端使用 UTF-8 编码 |

### 10.2 缩进规则

VUS 使用缩进表示代码块，类似 Python：
- 同一代码块必须使用相同缩进
- 建议使用 **4 个空格**
- 不要混用 Tab 和空格

```
# 正确缩进
如果 x > 0:
    打印("正数\n")      # 4 空格缩进
    如果 x > 100:
        打印("大数\n")  # 8 空格缩进

# 错误缩进
如果 x > 0:
    打印("正数\n")
     打印("错误\n")    # 缩进不一致
```

### 10.3 编码规范

- 所有源文件必须使用 **UTF-8** 编码
- 字符串字面量支持转义符：`\n`、`\t`、`\"`、`\\`、`\xHH`、`\uHHHH`
- 文件扩展名：`.vus`

### 10.4 运行测试

```bash
# 一键测试（安装时选择）
# 或手动运行：
vus run tests/test_hello.vus
vus run tests/test_demo.vus

# 运行全部测试
for f in tests/test_*.vus; do echo "--- $f ---"; vus run "$f"; done
```

---

## 附录

### A. 测试文件清单

| 文件 | 覆盖功能 | 状态 |
|------|---------|:----:|
| test_hello.vus | 基本输出 | ✅ 稳定 |
| test_variables.vus | 变量声明与赋值 | ✅ 稳定 |
| test_arithmetic.vus | 四则运算 | ✅ 稳定 |
| test_control.vus | 条件分支 | ✅ 稳定 |
| test_functions.vus | 函数定义与调用 | ✅ 稳定 |
| test_comparison.vus | 比较运算符 | ✅ 稳定 |
| test_string.vus | 字符串拼接与比较 | ✅ 稳定 |
| test_while_count.vus | 当循环 | ✅ 稳定 |
| test_factorial.vus | 递归阶乘 | ✅ 稳定 |
| test_fibonacci.vus | 递归斐波那契 | ✅ 稳定 |
| test_nested_control.vus | 嵌套循环 | ✅ 稳定 |
| test_comprehensive.vus | 综合功能 | ✅ 稳定 |
| test_demo.vus | 10 模块综合演示 | ✅ 稳定 |

### B. 项目结构

```
vus/
├── include/vus/       # 公共 API 头文件
│   ├── vus.h          # 核心类型和配置
│   ├── vus_abi.h      # C ABI 接口
│   ├── vus_plugin.h   # 插件系统接口（.vux）
│   ├── vus_lang.h     # 语言插件接口（.vulage）
│   └── vus_vusx.h     # VUS 插件接口（.vusx）
├── src/               # 编译器源码
│   ├── main.c         # CLI 入口
│   ├── lexer.c/h      # 词法分析器
│   ├── parser.c/h     # 语法分析器
│   ├── token.c/h      # Token 类型定义
│   ├── ast.c/h        # 抽象语法树
│   ├── generator.c/h  # 代码生成器
│   ├── config.c/h     # 配置加载
│   ├── vus_abi.c      # C ABI 实现
│   ├── vus_plugin.c   # .vux 插件系统实现
│   ├── vus_lang.c/h   # .vulage 语言插件系统实现
│   └── vus_vusx.c/h   # .vusx 插件系统实现
├── rt/                # 运行时库
│   ├── libvus_rt.h    # 运行时类型定义
│   └── libvus_rt.c    # 运行时实现
├── scripts/           # 工具脚本
│   ├── vux_plugin_manager.py  # 插件管理（安装/打包/列出）
│   └── vux_plugin_entry.py    # 插件基类
├── plugins/           # 插件目录
│   ├── lang/          # 语言插件（.vulage）
│   │   └── 易语言/    # 示例：易语言语法插件
│   └── func/          # 功能插件（.vux）
├── tests/             # 测试用例
├── examples/          # 示例程序
│   └── plugins/示例/  # 示例插件
├── docs/              # 文档
├── Makefile           # 构建系统
└── install.sh         # 安装脚本
```

### C. 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v0.1 | 2026-07 | 初始版本：词法分析、语法分析、代码生成、C ABI、插件系统 |