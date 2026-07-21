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
#define TYPE_MIXED   99

// ============ 前向声明 ============
typedef struct VusString VusString;
typedef struct VusList VusList;
typedef struct VusDict VusDict;
typedef struct VusClosure VusClosure;
typedef struct VusError VusError;

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

// ============ 栈追踪支持 ============
#define VUS_MAX_STACK_DEPTH 256
extern int vus_stack_depth;
extern const char* vus_stack_frames[VUS_MAX_STACK_DEPTH];

void vus_stack_push(const char* func_name);
void vus_stack_pop(void);
void vus_stack_print(void);

// ============ 标准库辅助函数 ============
// 以下函数由编译器生成的 C 代码调用，用于标准库功能

void vus_print(VusString* s);
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

// ============ 线程支持 ============
typedef struct VusThread VusThread;

VusThread* vus_thread_create(void* (*func)(void*), void* arg);
void* vus_thread_join(VusThread* thread);
void vus_thread_detach(VusThread* thread);

// ============ 异步 / 协程支持 ============
// 使用 ucontext 实现轻量级协程
typedef struct VusCoroutine VusCoroutine;

VusCoroutine* vus_coro_create(void (*func)(void*), void* arg);
void vus_coro_resume(VusCoroutine* coro);
void vus_coro_yield(void);
int vus_coro_is_done(VusCoroutine* coro);

#endif // VUS_RT_H