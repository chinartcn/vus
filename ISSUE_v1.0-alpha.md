# 📋 版本规划：v0.2 收尾 & v1.0-alpha 推进

## 当前完成状态

### v0.2 — 调试体验优化 + 预编译包 + 安装脚本（已全部完成 ✅）

- [x] 错误信息中文化（lexer / parser / vus_abi / main.c 全部中文，含文件名前缀）
- [x] 行号定位精准（parser 5 处错误消息新增列号，格式"第 %d 行第 %d 列"）
- [x] 运行时栈追踪（vus_stack_push/pop/print，深度上限 256）
- [x] 调试输出（`vus run --debug` 模式，vus_debug_enabled 标志）
- [x] 性能优化（缓冲区初始容量 4096→16384）
- [x] 预编译包（build_release.sh 支持 amd64/arm64/arm32 交叉编译）
- [x] 安装脚本（自动检测架构、预编译包优先、进度条、版本校验、6 个功能测试）
- [x] `vus update` 命令（Git 拉取 + 预编译包下载两种模式）

---

### v1.0-alpha — 泛型、结构体、多线程、异步、自举、apk 打包

#### 已完成的子任务

- [x] **泛型（语法级）** — `<T, U>` 泛型类型参数解析，生成器标记 `/* generic type params: T */`
  - 注意：当前仅语法标记，不支持类型参数化调用（如 `vus_T()` 会生成未定义函数）

- [x] **结构体** — 定义 / 实例化 / 成员访问 / 链式访问
  - `struct 名称:` 和 `结构 名称:` 双关键字
  - `StructName(args)` 构造语法，生成 calloc + 引用计数结构
  - `obj.field` 成员访问，C 代码生成 `((vus_struct_X*)obj)->vus_field`
  - 链式访问 `r.p1.x` 类型推导（通过字段类型注解递归解析中间类型）
  - 生成的 C struct 首字段为 `int ref;` 支持引用计数

- [x] **多线程** — 基于 pthread 的线程支持
  - `vus_thread_create/join/detach` 运行时函数
  - GCC 编译命令添加 `-lpthread` 链接标志

- [x] **异步协程** — 基于 ucontext 的协程支持
  - `vus_coro_create/resume/yield/is_done` 运行时函数
  - 静态调度器上下文 `vus_coro_main_ctx`

#### 待完成的子任务

- [ ] **自举（self-hosting）** — 用 VUS 编写编译器自身
  - 子任务 A：编写 VUS 版的词法分析器（lexer.vus）
  - 子任务 B：编写 VUS 版的语法分析器（parser.vus）
  - 子任务 C：编写 VUS 版的代码生成器（generator.vus）
  - 子任务 D：自举测试——用当前 VUS 编译器编译 VUS 版编译器
  - 子任务 E：用 VUS 版编译器再次编译自身，验证自举循环

- [ ] **apk 打包** — Android APK 构建支持
  - 子任务 A：调研 Android NDK 交叉编译方案
  - 子任务 B：添加 `vus build --apk` 命令
  - 子任务 C：生成 AndroidManifest.xml + 资源文件模板
  - 子任务 D：集成 aapt/apksigner 打包签名
  - 子任务 E：测试 APK 在 Android 设备上的运行

#### 已知问题 / 待修复

- [ ] 泛型函数调用 `创建<T>()` 生成 `vus_T()` 未定义函数
  - 当前泛型仅作为语法标记，不支持类型参数化调用
  - 需要在生成器中跟踪类型参数并生成正确的类型特化版本

---

## 版本路线总览

```
v0.1 → v0.2 → v1.0-alpha → v1.0-beta → v1.0 → v1.1 → ... → v1.9 → v2.0 → ...
  核心开发    语言能力扩展                质量优化     文档与体验 ...
```

## 参考资料

- 编译器设计规范：`docs/COMPILER_GUIDELINES.md`
- 教程文档：`docs/TUTORIAL.md`
- 安装脚本：`install.sh`
- 预编译包构建：`scripts/build_release.sh`
- 测试套件：`tests/run_tests.sh`