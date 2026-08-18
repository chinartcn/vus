#ifndef VUS_RT_H
#define VUS_RT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// ============ 类型标记常量 ============
#define TYPE_INT     1
#define TYPE_FLOAT   2
#define TYPE_STR     3
#define TYPE_BOOL    4
#define TYPE_LIST    5
#define TYPE_DICT    6
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

struct VusObject {
    int ref;    // 引用计数，必须为第一个字段（与 VusString/VusList/VusDict 约定一致）
    int magic;  // VUS_OBJECT_MAGIC：供 vus_print/vus_typeof 区分 VusObject 与 VusString
    int type;   // TYPE_LIST / TYPE_DICT / TYPE_STR / TYPE_INT / TYPE_FLOAT / TYPE_BOOL
    union {
        VusList*    list;
        VusDict*    dict;
        VusString*  str;
    } u;
};

// 判断指针是否为结构化容器（VusObject*）。非容器（普通 VusString*）返回 0。
// 依据：VusObject 在 ref 后有 magic 标记，而 VusString 在 ref 后是 len（小整数），
// 不会与 VUS_OBJECT_MAGIC 冲突。
static inline int vus_is_object(void* obj) {
    return obj && ((VusObject*)obj)->magic == VUS_OBJECT_MAGIC;
}

// 将任意值（VusString* 或 VusObject*）转为字符串表示：标量取原文，列表/字典递归序列化。
// 纯 C 实现，不依赖嵌入式 Python，供 vus_print 等安全消费。
VusString* vus_object_to_string(void* obj);

// ============ 引用计数通用操作 ============
void vus_ref(void* obj);
void vus_unref(void* obj);

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
VusString* vus_string_concat(VusString* a, VusString* b);
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
void vus_list_append(VusList* list, void* item);
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
    const char* msg;
    VusError* next;   // 错误链（最新错误在链头）
};

VusError* vus_error_new(int code, const char* msg, int line, const char* func);
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

/* 插件调用（调用 .vux Python 插件） */
VusString* vus_plugin_run_vux(VusString* plugin, VusString* cmd);

/* 进程内嵌入 Python 解释器（惰性 dlopen libpython），0 成功，-1 失败或未启用 VUS_USE_PY */
int vus_py_init(void);

/* 进程内调用 .vux 插件，返回字符串结果（VUS_USE_PY 下用嵌入解释器，否则回退子进程） */
VusString* vus_plugin_run_vux_inproc(VusString* plugin, VusString* cmd);

/* 进程内调用 .vux 插件，返回结构化 VusObject*（列表/字典/字符串），失败返回 NULL */
void* vus_plugin_run_vux_json(VusString* plugin, VusString* cmd);

/* JSON 字符串 -> 结构化 VusObject*（组合容器），失败返回 NULL */
void* vus_json_parse(VusString* s);

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

#endif // VUS_RT_H