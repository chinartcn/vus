> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


---
intent: 将 VUS 对 .vux Python 插件的调用从「子进程 + CLI」升级为「嵌入 Python 解释器、进程内直接调用」，并为插件返回值建立结构化数据（JSON/列表/字典）到 VUS 变量的传递机制。
success_criteria:
  - 插件_运行 通过进程内嵌入解释器调用插件，无 popen/CLI 子进程
  - 插件 run() 返回的结构化数据（dict/list）可写入 VUS 列表/字典变量
  - VUS 生成器支持列表/字典字面量（VUS_AST_LIST_LITERAL / VUS_AST_DICT_LITERAL）
  - 新内建函数（插件_运行JSON / JSON_解析 / JSON_生成）有单元测试覆盖
  - 关键路径（gcc 编译、生成器、运行时）通过 make test 回归
risk_level: high
auto_approve: false
branch: master
worktree: false
dirty_worktree: allow
---

## 步骤总览

本工作流将 VUS 插件调用从「子进程+CLI」升级为「嵌入 Python 解释器、进程内直接调用」，并建立结构化数据（JSON/列表/字典）传递机制。涉及四个文件族：运行时库（rt/）、生成器（src/generator.c）、编译链（Makefile + src/generator.c:vus_compile_c）、测试（tests/ + 新增 C 单测）。所有新增代码均以 `VUS_USE_PY` 宏为开关，缺解释器环境自动降级到现有子进程方案。

## Steps

- [x] **Step 1: 在运行时库头文件声明嵌入解释器与结构化数据接口**
action: 编辑 /workspace/vus/rt/libvus_rt.h，在「插件运行时函数」区段新增以下声明（置于现有 vus_plugin_run_vux 声明之后）：vus_py_init()（返回值 int，0 成功）、vus_plugin_run_vux_inproc(VusString* plugin, VusString* cmd)（返回 VusString*）、vus_plugin_run_vux_json(VusString* plugin, VusString* cmd)（返回 void*，指向 VusObject 容器）、vus_json_parse(VusString* s)（返回 void*）、vus_json_generate(void* obj)（返回 VusString*）、vus_typeof(void* obj)（返回 VusString*，类型名）。同时声明轻量**组合容器** struct VusObject：含 int type 与 union（成员为 VusList* / VusDict* / VusString* 指针），type 复用 TYPE_INT/TYPE_FLOAT/TYPE_STR/TYPE_BOOL 及新增 TYPE_LIST/TYPE_DICT 常量。不修改任何现有声明。
verify:
  type: shell
  command: grep -c "vus_plugin_run_vux_inproc\|vus_plugin_run_vux_json\|vus_json_parse\|vus_json_generate\|vus_py_init" /workspace/vus/rt/libvus_rt.h
gate: auto

- [ ] **Step 2: 运行时库实现惰性嵌入 Python 解释器（vus_py_init）**
action: 编辑 /workspace/vus/rt/libvus_rt.c，新增 static 段：声明函数指针 typedef 集合（Py_Initialize/PyFinalize/PyImport_ImportModule/PyObject_CallMethod/PyErr_Print/PyErr_Clear/PyString_AsString 等），实现 vus_py_init() 用 dlopen("libpython3.so", RTLD_NOW|RTLD_GLOBAL) 惰性加载并 dlsym 定位符号，成功则调用 Py_Initialize 并置全局 init 标志返回 0；dlopen 或 dlsym 任一失败则释放句柄返回 -1。所有 dl* 调用包在 #ifdef VUS_USE_PY 内，未定义时 vus_py_init 直接返回 -1。编译时需 -ldl（链接器已提供）。保持现有 vus_plugin_run_vux 子进程实现不动。
verify:
  type: shell
  command: grep -c "vus_py_init\|dlopen\|#ifdef VUS_USE_PY" /workspace/vus/rt/libvus_rt.c
gate: auto

- [ ] **Step 3: 实现 PyObject→Vus 对象转换与 JSON 解析生成**
action: 在 /workspace/vus/rt/libvus_rt.c 中新增以下函数：vus_py_to_vus(PyObject* obj) 将 Python 的 str/int/float/bool/list/dict 分别转换为 VusString/VusList/VusDict 并正确管理引用计数（str 用 PyString_AsStringAndSize，list 用 PyList_Size+PyList_GetItem，dict 用 PyDict_Next）；vus_json_parse(VusString* s) 内部调用 Python json.loads 返回的 PyObject 再经 vus_py_to_vus 转换；vus_json_generate(void* obj) 将 VusList/VusDict/VusString 经 Python json.dumps 序列化为 VusString*。所有函数仅在 VUS_USE_PY 下编译本体，否则返回空值。dict/list 元素需调用 vus_ref 保持引用。
verify:
  type: shell
  command: grep -c "vus_py_to_vus\|vus_json_parse\|vus_json_generate" /workspace/vus/rt/libvus_rt.c
gate: auto

- [ ] **Step 4: 实现进程内插件调用 vus_plugin_run_vux_inproc 与结构化 vus_plugin_run_vux_json**
action: 在 /workspace/vus/rt/libvus_rt.c 中实现 vus_plugin_run_vux_inproc(plugin, cmd)：调用 vus_py_init()，成功后用 PyImport_ImportModule 加载插件包（插件名经 VuxPlugin 协议解析，参考 scripts/vux_plugin_entry.py 的 run 流程），实例化插件类并调用 run(cmd)，将返回值经 vus_py_to_vus 转成 VusString（若返回 dict/list 则先 json.dumps 序列化）返回；失败时打印错误并返回空 VusString。实现 vus_plugin_run_vux_json(plugin, cmd)：同上但直接返回 vus_py_to_vus 的结构化 void* 容器。两者均包在 VUS_USE_PY 内；未定义时 vus_plugin_run_vux_inproc 回退到现有 vus_plugin_run_vux 的实现逻辑（即调用子进程），vus_plugin_run_vux_json 返回 NULL。不得修改现有 vus_plugin_run_vux 函数体。
verify:
  type: shell
  command: grep -c "vus_plugin_run_vux_inproc\|vus_plugin_run_vux_json" /workspace/vus/rt/libvus_rt.c
gate: auto

- [ ] **Step 5: 生成器支持列表/字典字面量**
action: 编辑 /workspace/vus/src/generator.c 的 gen_expr 函数（约 844 行起的 switch 分支区），新增 case VUS_AST_LIST_LITERAL 与 case VUS_AST_DICT_LITERAL。列表字面量生成形如 ((VusList*)vus_list_new(TYPE_MIXED)) 后逐元素调用 vus_list_append；字典字面量生成 vus_dict_new() 后逐键值调用 vus_dict_set。需先读取 ast.h 确认 VusAstListLiteral/VusAstDictLiteral 结构体字段名（elements/keys/values）再编码。产物字符串 strdup 返回。
verify:
  type: shell
  command: grep -c "VUS_AST_LIST_LITERAL\|VUS_AST_DICT_LITERAL" /workspace/vus/src/generator.c
gate: auto

- [ ] **Step 6: 生成器映射插件_运行JSON / JSON_解析 / JSON_生成 / typeof 内建函数**
action: 在 /workspace/vus/src/generator.c 的插件调用内置函数区段（约 680 行，现有 插件_运行 分支之后）新增：插件_运行 分支改为在 VUS_USE_PY 下输出 vus_plugin_run_vux_inproc(a,b)、否则维持 vus_plugin_run_vux(a,b)（用 #ifdef 包裹生成串）；新增 插件_运行JSON 分支输出 vus_plugin_run_vux_json(a,b)；新增 JSON_解析 分支输出 vus_json_parse(a)；新增 JSON_生成 分支输出 vus_json_generate(a)；新增 typeof 分支输出 vus_typeof(a)。每个分支依据 call->args->count 校验参数个数。
verify:
  type: shell
  command: grep -c "插件_运行JSON\|JSON_解析\|JSON_生成\|typeof\|vus_plugin_run_vux_inproc" /workspace/vus/src/generator.c
gate: auto

- [ ] **Step 7: 编译链注入 libpython 头文件与链接参数**
action: 修改两处。1) /workspace/vus/Makefile：新增 PY_INC 与 PY_LD 变量（用 shell 调 python3-config --includes 与 --ldflags 捕获，失败则置空），给 $(RT_OBJ) 与 $(RT_CORO_OBJ) 编译规则追加 -DVUS_USE_PY $(PY_INC)，给 vus 链接与 $(RT_LIB) 相关规则追加 $(PY_LD) 及 -Wl,-rpath 定位 libpython。2) /workspace/vus/src/generator.c:vus_compile_c（约 1719 行命令构建区）：检测 python3-config 存在，存在则往 gcc 命令追加 -DVUS_USE_PY、includes 与 ldflags 及 -Wl,-rpath。检测失败时保持现有命令不变（即降级）。不得硬编码具体 python 版本路径。
verify:
  type: shell
  command: grep -c "VUS_USE_PY\|python3-config" /workspace/vus/Makefile /workspace/vus/src/generator.c
gate: auto

- [ ] **Step 8: 新增插件调用与 JSON 转换的 C 单元测试**
action: 在 /workspace/vus/tests/ 新建 test_plugin_inproc.c，链接 rt 源文件编译成可执行后进行断言测试：1) vus_json_parse 解析 "{\"a\":1,\"b\":[1,2,3]}" 得到 VusDict 且可通过 vus_dict_get 取到 VusString "1" 与 VusList 长度 3；2) vus_json_generate 将构造的 VusList 序列化为合法 JSON 串；3) vus_plugin_run_vux_inproc 调用 examples/plugins/示例 插件返回非空字符串。同时在 tests/run_tests.sh 追加对该可执行文件的调用。若当前环境无 libpython 头文件，则本步骤退化为仅编译源码确认无语法错误（gcc -fsyntax-only），并记录该降级事实。
verify:
  type: shell
  command: cd /workspace/vus && gcc -fsyntax-only -Irt rt/libvus_rt.c
gate: human

- [ ] **Step 9: 编写结构化返回的 VUS 集成测试 test_plugin_run_json.vus**
action: 在 /workspace/vus/examples/ 新建 test_plugin_run_json.vus，内容：定义变量 结果 = 插件_运行JSON("示例", "echo")，然后 输出(结果) 或将其赋给一个列表/字典变量并遍历打印；同时用 列表 = [1,2,3] 与 字典 = {"a":1} 字面量验证生成器输出 vus_list_new/vus_dict_new。保存后运行 ./vus run examples/test_plugin_run_json.vus 确认无编译错误且能输出内容。
verify:
  type: shell
  command: cd /workspace/vus && ./vus run examples/test_plugin_run_json.vus
gate: human

- [ ] **Step 10: 回归验证 make test 与降级路径**
action: 在 /workspace/vus 目录运行 make clean && make test 确认编译器回归全部通过。随后模拟无解释器环境：将 VUS_USE_PY 相关注入临时移除（或用 gcc -U 覆盖）重新编译 libvus_rt.o 与一个最小 .vus，确认构建成功且插件_运行 回退到子进程路径仍可运行。确认成功后将改动还原为启用 VUS_USE_PY 状态。
verify:
  type: shell
  command: cd /workspace/vus && make clean && make test
gate: human