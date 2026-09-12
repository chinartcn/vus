#ifndef VUS_RT_H
#define VUS_RT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// FFI Bridge 公共结构体（VusRTValue/VusRTModule 等 C 域接口类型）
#include "../include/vus/vus_rt_bridge.h"

// ============ 类型标记常量 ============
#define TYPE_INT     1
#define TYPE_FLOAT   2
#define TYPE_STR     3
#define TYPE_BOOL    4
#define TYPE_LIST    5
#define TYPE_DICT    6
#define TYPE_FUNC    7
#define TYPE_MIXED   99

// ============ 前向声明 ============
typedef struct VusString VusString;
typedef struct VusList VusList;
typedef struct VusDict VusDict;
typedef struct VusClosure VusClosure;
typedef struct VusError VusError;
typedef struct VusObject VusObject;

// ============ 结构化对象（组合容器） ============
// 带类型标记的轻量容器：内部 union 持有指向 VusList/VusDict/VusString 的指针。
// 不替代、不重写既有结构，用于承载插件返回的结构化数据。
#define VUS_OBJECT_MAGIC 0x564F4221  // 'VOB!'：运行时识别结构化容器的魔数

// ============ C 结构体实例（脚本「结构」） ============
// 生成器把脚本结构体编译为 C 结构体类型 vus_struct_<名>，实例以 VusString*
// 槽位存放（ref 必须为首字段，与 VusString 约定一致）。运行时 vus_unref 需
// 区分「普通 VusString」与「结构体实例」，因此生成器在 ref 后固定排放：
//   int ref;  int _magic;  void (*_release)(void*);  VusString* vus_<字段>...
// _magic == VUS_STRUCT_MAGIC（'VUS!'）标识结构体实例（VusString 第二字段是
// len 小整数，不会与其冲突）；_release 指向生成器为该结构体生成的字段递归
// 释放函数（B1：结构体归零释放时必须先 vus_unref 各字段引用，再 free 主体）。
// ============ 线程模型（B4） ============
// VUS 运行时的进程级共享状态（字面量池 g_lit_pool、驻留池 g_intern_keys、
// 数字串池 g_numstr_pool、容器释放链 g_rel_chain）已在内部用进程级递归互斥锁
// 保护（多线程各自构造/释放不同对象安全）。但引用计数（ref 增减）与池借出
// 对象的生命周期遵循**单线程契约**：
//   1) 同一 VUS 对象（含 vus_literal/vus_string_intern/vus_to_string 借出的
//      驻留实例）不得被多个线程同时 vus_ref/vus_unref；
//   2) vus_ref/vus_unref 对全局可共享对象需由宿主在进程级互斥下调用；
//   3) 脚本内建的 vus_literal/vus_to_string 高频借用保持单线程使用。
// 多线程各用各的对象、不在线程间传递变量引用时无需额外同步。
#define VUS_STRUCT_MAGIC 0x56555321  // 'VUS!'：运行时识别 C 结构体实例的魔数

typedef struct {
    int  ref;                          // 引用计数（首字段）
    int  magic;                        // VUS_STRUCT_MAGIC
    void (*release)(void*);            // 生成器提供的字段递归释放函数
} VusStructHeader;

struct VusObject {
    int ref;    // 引用计数，必须为第一个字段（与 VusString/VusList/VusDict 约定一致）
    int magic;  // VUS_OBJECT_MAGIC：供 vus_print/vus_typeof 区分 VusObject 与 VusString
    int type;   // TYPE_LIST / TYPE_DICT / TYPE_STR / TYPE_INT / TYPE_FLOAT / TYPE_BOOL / TYPE_FUNC
    union {
        VusList*    list;
        VusDict*    dict;
        VusString*  str;
        void (*fn)(void*);   /* TYPE_FUNC：裸函数指针（VUS 函数一等公民载体） */
    } u;
};

// 判断指针是否为结构化容器（VusObject*）。非容器（普通 VusString*）返回 0。
// 依据：VusObject 在 ref 后有 magic 标记，而 VusString 在 ref 后是 len（小整数），
// 不会与 VUS_OBJECT_MAGIC 冲突。
static inline int vus_is_object(void* obj) {
    return obj && ((VusObject*)obj)->magic == VUS_OBJECT_MAGIC;
}

// 判断指针是否为任何容器（VusObject* 或结构体实例）。非容器（普通 VusString*）返回 0。
// VusString 的 ref 后第二字段是 len（小正整数），不会与两个大魔数冲突。
// 用于 R6 返回值收割点：仅对非容器（NUL 字符串或标量）归还出生引用，容器出生 ref=0
// 精确转让（var_set +1 即唯一持有），豁免避免提前释放。
static inline int vus_is_container(void* obj) {
    if (!obj) return 0;
    int m = ((int*)obj)[1];
    return m == VUS_OBJECT_MAGIC || m == VUS_STRUCT_MAGIC;
}

// 将任意值（VusString* 或 VusObject*）转为字符串表示：标量取原文，列表/字典递归序列化。
// 纯 C 实现，不依赖嵌入式 Python，供 vus_print 等安全消费。
VusString* vus_object_to_string(void* obj);

// ============ 引用计数通用操作 ============
void vus_ref(void* obj);
void vus_unref(void* obj);
/* 变量赋值（生成器热模板）：*slot = v，正确处理引用计数（vus_ref 新值、
 * unref 旧值；两者均容忍 NULL）。把生成器 4 行内联收敛为一行调用。 */
void vus_var_set(VusString** slot, void* v);

// ============ 字符串 ============
// data 始终以 '\0' 结尾（便于 C 互操作），但字符串内容可能包含中间 '\0'。
// len 表示有效字节长度（不含结尾 '\0'），保证 len <= strlen(data)。
struct VusString {
    int ref;
    int len;          // UTF-8 字节长度
    char* data;       // 只读字符缓冲区
};

VusString* vus_string_new(const char* s);
VusString* vus_string_new_len(const char* s, int len);
/* 字符串驻留：高频重复键名/常量返回缓存实例（借用语义，调用方 vus_unref 归还） */
VusString* vus_string_intern(const char* s);
/* 字符串常量字面量池：同内容的不可变字面量返回同一常驻实例（借用，调用方不
 * 释放、不修改内容）。用于生成器把源码字符串字面量替换成驻留引用，避免高频
 * 路径每次 malloc+复制；池持有保底引用，借出不增加引用计数。 */
VusString* vus_literal(const char* s);
VusString* vus_string_concat(VusString* a, VusString* b);
/* R3：多段拼接一次分配（a..b..c 链折叠 → 单块 malloc + n 次 memcpy）。
 * parts 为 n 个操作数（容忍 NULL 段），返回 ref=1 的新串。 */
VusString* vus_string_concat_n(VusString** parts, int n);
VusString* vus_string_slice(VusString* s, int start, int len);
int vus_string_len(VusString* s);
char* vus_string_cstr(VusString* s);  // 返回的指针指向 data，调用方禁止修改或释放

// ============ 列表 ============
struct VusList {
    int ref;
    int len;
    int cap;
    void** items;
    int type;          // 元素类型标记（严格模式）/ TYPE_MIXED（混合模式）
};

VusList* vus_list_new(int type);
VusList* vus_list_unwrap(void* obj);
VusDict* vus_dict_unwrap(void* obj);
void vus_list_append(VusList* list, void* item);
/* 字面量装箱 helper：创建 VusObject(TYPE_LIST/TYPE_DICT)，供生成器精简
 * 列表/字典字面量（ref 初始 0，与生成器旧内联模板等价）。 */
VusObject* vus_object_list(void);
VusObject* vus_object_dict(void);
VusObject* vus_object_func(void (*fn)(void*));
VusString* vus_object_func_call(VusObject* o, VusString** args);
void* vus_list_get(VusList* list, int index);
void vus_list_remove(VusList* list, int index);
void vus_list_set(VusList* list, int index, void* item);
int vus_list_len(VusList* list);

// ============ 字典 ============
// 键仅支持 VusString*（字符串），值可为任意 VUS 对象
// 哈希表实现策略：链地址法（separate chaining），负载因子超过 0.75 时自动扩容
// v0.1 不提供字典遍历接口，v1.0 补充
struct VusDict {
    int ref;
    void* impl;       // 哈希表实现（运行时库内部）
};

VusDict* vus_dict_new(void);
void vus_dict_set(VusDict* dict, VusString* key, void* value);
void* vus_dict_get(VusDict* dict, VusString* key);
void vus_dict_remove(VusDict* dict, VusString* key);
int vus_dict_len(VusDict* dict);

/* 返回字典所有键（VusString*）构成的列表（内部新建，调用方负责 vus_unref）。
 * 元素为键副本，供遍历（v0.1 补充的遍历接口）。 */
VusList* vus_dict_keys(VusDict* dict);

/* 取结构化字典（VusObject* 的 TYPE_DICT）的键列表；非字典容器返回空列表。
 * 供脚本内建「字典_键」使用，避免误把任意对象当 VusDict 解引用。 */
VusList* vus_dict_keys_of(void* obj);

/* 取结构化字典（VusObject* 的 TYPE_DICT）的全部键值对：返回 VusObject*(TYPE_LIST)，
 * 每个元素又是一个 VusObject*(TYPE_LIST)「[键, 值]」双元素列表。
 * 供脚本内建「字典_项」使用：可配合「循环 对 在 字典_项(字典)」同时拿到键与值。
 * 非字典容器返回空列表。调用方按 VUS 引用计数规则负责释放返回对象。 */
VusObject* vus_dict_items(void* obj);

/* ---- Termux-X11 一键启动（Termux_* 内建） ----
 * 在脚本里免去手动敲 termux-x11/环境变量/virgl_test_server 的命令。 */
int vus_termux_start_x11(void);  /* 启动 termux-x11 :0 并设 DISPLAY=:0 */
int vus_termux_start_gl(void);   /* 灌入 zink MESA/GALLIUM 环境变量并启 virgl_test_server */

// ============ 闭包 ============
// 闭包参数约定：args 由调用方分配，调用结束后由调用方负责释放（vus_unref）。
// func 在调用期间持有 args 的引用，但不负责释放。
struct VusClosure {
    int ref;
    void (*func)(void* env, void* args);
    void* env;
};

VusClosure* vus_closure_new(void (*func)(void*, void*), void* env);
void vus_closure_call(VusClosure* closure, void* args);

// ============ 错误处理（错误码链） ============
// 注意：VusError 不参与引用计数，由运行时库独立管理，
// 用户无需调用 vus_ref/vus_unref。
struct VusError {
    int code;
    int line;
    const char* func;
    const char* type;   // 异常类型名（如 值错误/网络错误；无类型时默认 "错误"）
    const char* msg;
    VusError* next;   // 错误链（最新错误在链头）
};

VusError* vus_error_new(int code, const char* msg, int line, const char* func);
/* 带类型的异常：type 为异常类型名（NULL 记默认 "错误"）；旧 vus_error_new 等价于 type="错误" */
VusError* vus_error_new_typed(int code, const char* type, const char* msg, int line, const char* func);
/* except 类型匹配：name == type 或 name == msg（兼容旧「消息即类型」用法）均命中 */
int vus_error_matches(VusError* err, const char* name);
void vus_error_push(VusError** chain, VusError* err);
void vus_error_print(VusError* err);
void vus_error_free(VusError* err);

// ============ 调试支持 ============
extern int vus_debug_enabled;
void vus_debug_print(const char* msg);

// ============ 分级日志（EasyLogger 集成） ============
// 首次调用任一 日志_* 内建函数时惰性初始化；无需手动调用 vus_log_init。
// 4 级输出函数返回 VusString*（"0" 成功 / "-1" 失败），沿用内建函数约定。

int vus_log_init(void);                              // 初始化 EasyLogger，幂等，失败返回 -1
VusString* vus_log_set_level(VusString* level);      // 设置过滤级别（调试/信息/警告/错误）
VusString* vus_log_debug(VusString* msg);
VusString* vus_log_info(VusString* msg);
VusString* vus_log_warn(VusString* msg);
VusString* vus_log_error(VusString* msg);

// ============ 栈追踪支持 ============
#define VUS_MAX_STACK_DEPTH 256
extern int vus_stack_depth;
extern const char* vus_stack_frames[VUS_MAX_STACK_DEPTH];

void vus_stack_push(const char* func_name);
void vus_stack_pop(void);
void vus_stack_print(void);

// ============ 标准库辅助函数 ============
// 以下函数由编译器生成的 C 代码调用，用于标准库功能

void vus_print(void* s);
VusString* vus_input(VusString* prompt);

// vus_add：加法/字符串拼接。若两个操作数均可解析为整数则做算术加法，否则做字符串拼接。
VusString* vus_add(VusString* a, VusString* b);

// vus_to_int：字符串转整数。成功时返回结果，*err 置 0。
// 失败时返回 0，*err 置非 0（调用方应检查 err 并抛出异常）。
int64_t vus_to_int(VusString* s, int* err);

VusString* vus_to_string(int64_t n);

// vus_compare：比较两个字符串。两者均可解析为整数时按数值比较，否则按字典序。
// 返回 -1 / 0 / 1，供编译器生成的 == != < > <= >= 使用。
int vus_compare(VusString* a, VusString* b);

// vus_to_float：字符串转浮点数。成功时返回结果，*err 置 0。
// 失败时返回 0.0，*err 置非 0。
double vus_to_float(VusString* s, int* err);

// ============ VUS XYZ 体感音游内建（rt/vus_xyz.c） ============
// 全部统一返回 VusString*，可作表达式或语句。数值约定 milli-g 整数（1g=1000）。
VusString* vus_clock_ms(void);                       /* 时钟()：单调毫秒 */
VusString* vus_sensor_read(const char* axis);        /* 传感器_读("x"/"y"/"z")：毫 g */
VusString* vus_audio_open(const char* path);         /* 音频_打开(path) */
VusString* vus_audio_play(void);                     /* 音频_播放() */
VusString* vus_audio_pause(void);                    /* 音频_暂停() */
VusString* vus_audio_resume(void);                   /* 音频_续() */
VusString* vus_audio_seek(int64_t ms);               /* 音频_跳转(ms) */
VusString* vus_audio_position(void);                 /* 音频_进度()：毫秒 */
VusString* vus_audio_duration(void);                 /* 音频_时长()：毫秒 */

// ============ 线程支持 ============
typedef struct VusThread VusThread;

VusThread* vus_thread_create(void* (*func)(void*), void* arg);
void* vus_thread_join(VusThread* thread);
void vus_thread_detach(VusThread* thread);

/* 睡眠：休眠 ms 毫秒（生成器把 睡眠(ms) 映射为 vus_thread_sleep(vus_to_string(ms))）。 */
void vus_thread_sleep(VusString* ms);

// ============ 命令行参数支持（自举编译器 CLI 用） ============
// 生成器把 命令行_参数数() 映射为 vus_cli_argc()，命令行_参数(i) 映射为 vus_cli_argv(...)。
void vus_cli_init(int argc, char** argv);           /* main 开头调用，注入 argc/argv */
VusString* vus_cli_argc(void);                       /* 返回参数个数（含程序名） */
VusString* vus_cli_argv(VusString* index);           /* 返回第 index 个参数 */

// ============ Java 平台能力桥（网络/文件由 Java 暴露、VUS 调用） ============
// JNI 宿主（APK）通过本函数注册回调：VUS 的 网络_* / 文件_* 内建在 APK 内优先走
// Java 实现（用户架构约定：平台能力放 Java 平台层）；桌面/纯 native 环境未注册时
// 各内建自动回退到 native 内建实现（stdio/curl）。未注册时本函数无副作用。
// 回调契约：api=能力名（如 "file.read"/"http.get"），args=JSON 参数串，
// out 指向 malloc 缓冲（Java 返回 JSON：{"ok":1,"data":"..."} / {"ok":0,"err":"..."}）。
void vus_set_java_callback(void (*fn)(const char *api, const char *args, char **out));

/* 线程/协程句柄接口（返回 VusString* 句柄，避免指针类型转换问题） */
#define VUS_MAX_HANDLES 64
VusString* vus_thread_create_handle(void* (*func)(void*), void* arg);
void* vus_thread_join_handle(VusString* handle);
VusString* vus_coro_create_handle(void (*func)(void*), void* arg);
void vus_coro_resume_handle(VusString* handle);

// ============ 异步 / 协程支持 ============
// 使用 ucontext 实现轻量级协程
typedef struct VusCoroutine VusCoroutine;

VusCoroutine* vus_coro_create(void (*func)(void*), void* arg);
void vus_coro_resume(VusCoroutine* coro);
void vus_coro_yield(void);
int vus_coro_is_done(VusCoroutine* coro);

// 真 await：驱动协程句柄到完成并返回其返回值；vus_coro_store_result 让协程存结果
VusString* vus_coro_await_handle(VusString* handle);
void vus_coro_store_result(void* result);

// ============ 插件运行时函数（VusString* 接口） ============

/* TUI（使用 ANSI 转义码） */
VusString* vus_plugin_tui_clear(VusString* dummy);
VusString* vus_plugin_tui_set_color(VusString* fg, VusString* bg);
VusString* vus_plugin_tui_locate(VusString* row, VusString* col);
VusString* vus_plugin_tui_progress(VusString* current, VusString* total, VusString* width);
VusString* vus_plugin_tui_reset(VusString* dummy);

/* 网络（基于 libcurl） */
VusString* vus_plugin_http_get(VusString* url);
VusString* vus_plugin_http_post(VusString* url, VusString* data);
VusString* vus_plugin_http_download(VusString* url, VusString* filepath);

/* DEX 逻辑拓展（仅 APK）：转 Java 平台桥 ext.* 命名空间，原样返回响应 JSON 串 */
VusString* vus_plugin_ext_call(VusString* plugin_op, VusString* args);

/* 热更协议（仅 APK）：应用含新 .so/.vua/.dex 的更新包（UpdateManager.applyUpdate）。
 * 返回 data：0=已应用(实时层生效, .so 重启生效) 1=无更新 -1=宿主过低 -2=失败 */
VusString* vus_plugin_hotupdate_apply(VusString* url);

/* 插件调用（调用 .vux Python 插件） */
VusString* vus_plugin_run_vux(VusString* plugin, VusString* cmd);

/* 进程内嵌入 Python 解释器（惰性 dlopen libpython），0 成功，-1 失败或未启用 VUS_USE_PY */
int vus_py_init(void);

/* 进程内调用 .vux 插件，返回字符串结果（VUS_USE_PY 下用嵌入解释器，否则回退子进程） */
VusString* vus_plugin_run_vux_inproc(VusString* plugin, VusString* cmd);

/* 进程内调用 .vux 插件，返回结构化 VusObject*（列表/字典/字符串），失败返回 NULL */
void* vus_plugin_run_vux_json(VusString* plugin, VusString* cmd);

/* JSON 字符串 -> 结构化 VusObject*（组合容器），失败返回 NULL。
 * 幂等：输入已是列表/字典（VusObject* TYPE_LIST/TYPE_DICT）时 ref+1 原样返回，
 * 使旧写法「JSON_解析(文本_分割(...))」在文本_分割 直接返回列表后依旧可用。 */
void* vus_json_parse(void* s);

/* 结构化 VusObject* -> JSON 字符串，失败返回空串 */
VusString* vus_json_generate(void* obj);

/* 根据路径查询 JSON（path 形如 "a.b[0].c"），返回结构化结果，失败返回 NULL */
void* vus_json_query(VusString* json, VusString* path);

/* 返回结构化值的类型名（整数/浮点/字符串/布尔/列表/字典/空） */
VusString* vus_typeof(void* obj);

/* 文件操作 */
VusString* vus_plugin_file_read(VusString* path);
VusString* vus_plugin_file_write(VusString* path, VusString* content);
VusString* vus_plugin_file_append(VusString* path, VusString* content);
VusString* vus_plugin_file_exists(VusString* path);
VusString* vus_plugin_file_delete(VusString* path);
VusString* vus_plugin_file_list(VusString* path);
/* 判断路径是否为目录：返回 "true"/"false"（供文件管理器区分文件与目录） */
VusString* vus_plugin_file_isdir(VusString* path);

/* ---------------- Android 轻量能力（APK 走 Java 平台桥；桌面无害降级） ----------------
 * 纯 Java + JNI 参数内建：振动/剪贴板/设备信息/Toast 由 VuaBridge.callJava 实现，
 * 桌面侧无对应物时降级返回无害值，脚本无需分支。 */
VusString* vus_plugin_vibrate(VusString* ms);             /* 振动(毫秒)：APK 真振，桌面 "0" */
VusString* vus_plugin_clipboard_read(void);               /* 剪贴板_读()：APK 读系统剪贴板，桌面 "" */
VusString* vus_plugin_clipboard_write(VusString* text);   /* 剪贴板_写(文本)：APK 写系统剪贴板，桌面 "0" */
VusString* vus_plugin_device_info(void);                  /* 设备_信息()：APK 返回 JSON，桌面固定 JSON */
VusString* vus_plugin_toast(VusString* text, VusString* is_long); /* 提示(文本, 时长)：APK 真 Toast，桌面 "0" */

/* 系统能力延伸（同一平台桥） */
VusString* vus_plugin_share_text(VusString* text);        /* 分享_文本(文本)：APK 调系统分享面板，桌面 "0" */
VusString* vus_plugin_battery_status(void);               /* 电源_电量()：APK 返回 {"电量":N,"充电中":b}，桌面固定 JSON */
VusString* vus_plugin_screen_keepon(VusString* flag);     /* 屏幕_常亮(开关)：APK 切窗口常亮标志，桌面 "0" */
VusString* vus_plugin_network_type(void);                 /* 网络_类型()：APK 返回 wifi/mobile/none，桌面 "none" */
VusString* vus_plugin_notify_send(VusString* title, VusString* body); /* 通知_发送(标题, 内容)：APK 发通知栏，桌面 "0" */

/* 主题（APK） */
VusString* vus_plugin_theme_set(VusString* name);   /* 主题_设置(浅色|暗色|跟随系统)：APK 切主题并重建，桌面 "0" */
VusString* vus_plugin_theme_get(void);              /* 主题_查询()：APK 返回当前主题名，桌面 "浅色" */
VusString* vus_plugin_theme_primary(VusString* color); /* 主题_主色：#RRGGBB/动态/默认：APK 换活力色并重建，桌面 "0" */

/* 媒体播放（APK Java 平台桥；桌面无害降级）
 * 音乐_播放(源, 循环, 音量) / 音乐_停止 / 音乐_暂停 / 音乐_继续 /
 * 音乐_跳转(秒) / 音乐_状态() / 视频_播放(源)。源可为本地相对/绝对路径或 http(s):// URL。 */
VusString* vus_plugin_media_play(VusString* src, VusString* loop, VusString* volume);
VusString* vus_plugin_media_stop(void);
VusString* vus_plugin_media_pause(void);
VusString* vus_plugin_media_resume(void);
VusString* vus_plugin_media_seek(VusString* pos);
VusString* vus_plugin_media_status(void);
VusString* vus_plugin_video_play(VusString* src);

/* shell 命令执行：popen 捕获命令标准输出，返回输出文本(上限 64KB) */
VusString* vus_plugin_shell_exec(VusString* cmd);

/* 文本分割：按分隔符直接拆成列表（VusObject* TYPE_LIST，元素 VusString*），
 * 免去原先「JSON 数组字符串 → JSON_解析 → 列表」三步（P7）。
 * 兼容说明：旧写法 JSON_解析(文本_分割(...)) 依旧可用 —— vus_json_parse 对
 * 已是列表/字典的输入幂等返回。 */
void* vus_plugin_text_split(VusString* text, VusString* sep);
/* 文本_行：按换行符 \n 拆成列表的一步到位函数（元素 VusString*，含末尾空段） */
void* vus_plugin_text_lines(VusString* text);

/* 通用网络请求（认证/超时/重试）：网络_请求(方式, 地址, 头JSON, 数据, 超时秒, 重试次数)
 * APK 走 Java 平台桥（headers 自定义请求头如 token、timeout、retry）；桌面回退 curl。 */
VusString* vus_plugin_http_request(VusString* method, VusString* url,
                                   VusString* headers_json, VusString* body,
                                   VusString* timeout_s, VusString* retry_s);
/* 文件上传（multipart/form-data）：文件_上传(地址, 本地文件, 字段JSON, 头JSON)
 * APK 走 Java 平台桥；桌面回退 curl -F（仅文件）。 */
VusString* vus_plugin_http_upload(VusString* url, VusString* path,
                                  VusString* fields_json, VusString* headers_json);

/* 日期时间 */
VusString* vus_plugin_date_now(VusString* dummy);
VusString* vus_plugin_date_format(VusString* fmt);
VusString* vus_plugin_date_parse(VusString* str, VusString* fmt);
VusString* vus_plugin_date_timestamp(VusString* dummy);
VusString* vus_plugin_date_from_timestamp(VusString* ts);
VusString* vus_plugin_date_year(VusString* dummy);
VusString* vus_plugin_date_month(VusString* dummy);
VusString* vus_plugin_date_day(VusString* dummy);
VusString* vus_plugin_date_hour(VusString* dummy);
VusString* vus_plugin_date_minute(VusString* dummy);
VusString* vus_plugin_date_second(VusString* dummy);

/* ============ 旧式标准库辅助函数（设计文档 §10.1 核心库接线） ============ */
/* 长度：列表/字典返回元素个数，其余（含字符串）返回 UTF-8 字节长度 */
VusString* vus_length(void* obj);
/* 替换：把 text 中所有 old_s 出现替换为 rep，返回新字符串 */
VusString* vus_string_replace(VusString* text, VusString* old_s, VusString* rep);
/* 取随机数：[最小值, 最大值] 闭区间随机整数 */
VusString* vus_random_int(VusString* min_s, VusString* max_s);
/* 断言失败：打印消息并以退出码 1 终止进程 */
void vus_assert_fail(VusString* msg);

/* ============ 哈希（vus_hash.c：SHA-256/MD5 纯 C） ============
 * 返回小写十六进制串；输入按 VusString len 处理（二进制安全）。 */
VusString* vus_hash_sha256(VusString* data);
VusString* vus_hash_md5(VusString* data);

/* ============ ZIP（vus_zip.c：miniz 封装） ============
 * 解压_zip(zip路径, 目标目录)：自动创建目录、拒绝 zip-slip 条目，返回 "0"/"-1"。
 * 压缩_zip(文件列表, zip路径)：listobj 为 VUS 列表（元素为文件路径串），
 * 条目名取 basename，返回 "0"/"-1"。 */
VusString* vus_zip_unzip(VusString* zippath, VusString* dest);
VusString* vus_zip_compress_list(void* listobj, VusString* zippath);
/* C 内部接口（vus_vaz.c 解包 .vaz 专用）：0 成功 / -1 失败 */
int vus_zip_unzip_to_dir(const char* zippath, const char* dest);

/* ============ 图片导出（vus_img.c：stb_image_write 封装） ============
 * 像素数据为原始字节缓冲（通道 3=RGB / 4=RGBA），长度须等于 w*h*comp；
 * PNG/BMP 返回 "0"/"-1"；JPG 附加质量 1~100。 */
VusString* vus_img_png_save(VusString* path, int w, int h, VusString* data, int comp);
VusString* vus_img_jpg_save(VusString* path, int w, int h, VusString* data, int quality, int comp);
VusString* vus_img_bmp_save(VusString* path, int w, int h, VusString* data, int comp);

/* ============ 正则（vus_regex.c：Oniguruma 封装，UTF-8） ============
 * 正则_查找(文本, 模式) → "1"/"0"；
 * 正则_匹配(文本, 模式) → 第一个完整匹配串，失败返回空串；
 * 正则_替换(文本, 模式, 替换) → 全部替换，替换串支持 \0~\9 分组引用。 */
VusString* vus_regex_find(VusString* text, VusString* pattern);
VusString* vus_regex_match(VusString* text, VusString* pattern);
VusString* vus_regex_replace(VusString* text, VusString* pattern, VusString* repl);

/* ============ 运行期外部桥（FFI Bridge）生成代码入口 ============
 * VUS 侧 导入外部/导出变量/别名.成员 语法编译为对下列入口的调用。
 * 值级约定：*out_vus 成功时为 VUS 对象（标量 ref=1 出生引用借出、容器 ref=0
 * 精确转让、NIL/空为 NULL），与 R6 收割语义一致；失败返回 -1 且 *err_out 为
 * 新建 VusError（类型 "外部错误"，msg 为域侧错误文本），由生成代码挂入 _vus_err。 */
int vus_ext_declare_v(const char *ns, const char *src, void *params_vus);       /* 导入外部（惰性声明，不加载） */
int vus_ext_call_v(const char *ns, const char *fname, void *const *args, int nargs,
                   void **out_vus, VusError **err_out);                          /* 别名.函数(实参) */
int vus_ext_get_v(const char *ns, const char *vname, void **out_vus, VusError **err_out); /* 别名.变量（读） */
int vus_ext_set_v(const char *ns, const char *vname, void *val_vus, VusError **err_out);  /* 别名.变量 = 值（写） */
int vus_ext_export_v(const char *name, void **ptr);                              /* 导出变量：注册全局槽 */
const char *vus_ext_last_error(void);                                            /* 最近一次桥错误文本 */
void vus_ext_shutdown_all(void);                                                 /* 程序退出：清理全部外部域 */

/* ---- C 域接入访问器（rt/vus_rt_c_impl.c 使用；尾部扩展，不改既有布局） ---- */
void vus_ext_seterr(const char *fmt, ...);                 /* 写最近桥错误文本（与 vus_ext_last_error 配对） */
int vus_ext_c_register(const char *ns, VusRTModule *mod, void *handle);  /* 登记 C 域模块 + dlopen 句柄（幂等） */
VusRTModule *vus_ext_c_module(const char *ns);             /* 按别名取 C 域模块描述符（未加载返回 NULL） */
void *vus_ext_c_handle(const char *ns);                    /* 按别名取 C 域 dlopen 句柄 */

#endif // VUS_RT_H