/*
 * vua.h — VUA 界面运行时（Android 组件流，多屏）
 *
 * 在 native 侧把 .vua（界面定义）解析为组件树、校验、产出"规范化渲染树"
 * 交给 Java 去建 Android View；并负责变量状态与"事件 → .vus 逻辑函数"的派发。
 * 多屏：VuaSession 管理屏栈，每屏 = 一个 .vua；.vus 用界面_显示/界面_返回 导航。
 *
 * 本模块存放在 rt/，与 libvus_rt 并列：复用其 VusString / VusDict / VusClosure，
 * 复用它已内嵌的 rt/yyjson（纯 C JSON 库，MIT）。由 vus_apk.c 生成壳编入 APK
 * 的 native 部分（LOCAL_SRC_FILES 加入 rt/vua.c）。
 *
 * 本模块是"独立 native 运行时"，不依赖任何 UI 框架；它只解析/校验/产树/派发，
 * 渲染与触摸仍在 Java 侧（APK shell）。详见 docs/VUA_REFERENCE.md 与
 * docs/VUA_RENDER_TREE.md 与 docs/API_REFERENCE.md 第 13 节。
 */

#ifndef VUS_VUA_H
#define VUS_VUA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "libvus_rt.h"   /* VusString / VusDict / VusClosure / VusObject */

/* ============ 错误 ============ */

/* VUA 错误码链（不参与引用计数，与 VusError 一致；调用者只读） */
typedef struct VuaError {
    int   code;                       /* 见下方 VUA_ERR_* */
    int   line;                       /* .vua 内行号（无则 0） */
    char  file[256];                  /* 出错文件（.vua / 控件表 / 词典） */
    char  msg[512];                   /* 中文错误信息 */
    struct VuaError *next;            /* 下一个（最新错误在链头） */
} VuaError;

#define VUA_ERR_NONE           0
#define VUA_ERR_JSON          -1   /* .vua 不是合法 JSON */
#define VUA_ERR_ROOT          -2   /* 根节点 type 不是「界面」 */
#define VUA_ERR_UNKNOWN_TYPE  -3   /* type 不在控件表 */
#define VUA_ERR_UNKNOWN_FIELD -4   /* 属性在词典查不到 */
#define VUA_ERR_BINDING       -5   /* 离线校验：事件名绑定不到 .vus 函数 */
#define VUA_ERR_SO_MISSING    -6   /* 控件依赖 .so 在打包清单中不存在 */

/* ============ 会话（session）：多屏/屏栈 ============ */

/* 一个 VUA 会话：管理整条屏栈与 session 级全局变量。内部结构 opaque。 */
typedef struct VuaSession VuaSession;
/* 一屏：组件树 + 变量状态 + 事件表。此处仅前置声明，供会话导航函数返回。 */
typedef struct VuaScreen VuaScreen;
/* 渲染树变量插槽（G3）：变量名 + 缺省显示值，供骨架占位符替换使用。内部结构 opaque。 */
typedef struct VuaVarSlot VuaVarSlot;

/*
 * 新建会话。控件表/词典在 session 级共享（见 vua_control_table_load / vua_dict_load，
 * 会话创建后任一屏解析前调用一次生效）。失败返回 NULL 并把错误挂到 *err。
 */
VuaSession *vua_session_new(VuaError *err);

/* 释放会话：释放屏栈内所有屏、全局变量、控件表/词典。 */
void vua_session_free(VuaSession *session);

/* 当前屏（栈顶）；会话无任何屏时返回 NULL。 */
VuaScreen *vua_session_current(VuaSession *session);

/*
 * 多屏导航：读取并解析一个 .vua，压栈成为当前屏。
 * 内部：读文件 → vua_screen_load → 入栈 → 返回新屏。
 * 成功返回新屏；失败返回 NULL 并挂 *err。
 */
VuaScreen *vua_session_show(VuaSession *session, const char *vua_path, VuaError *err);

/*
 * 多屏导航：把 .vua 格式的 JSON 字符串直接解析为一屏并压栈（动态渲染树）。
 * 供 .vus 运行时根据数据动态生成界面（如从网络/文件拉取数据后拼接渲染树）。
 * 成功返回 0；失败返回 -1（错误挂 *err，可为 NULL）。
 */
int vua_show_json(VuaSession *session, const char *vua_json, VuaError *err);

/* 多屏导航：弹栈回上一屏并返回它；已在最底屏时不弹，返回当前屏。 */
VuaScreen *vua_session_back(VuaSession *session);

/*
 * 多屏导航：弹到指定 .vua 名的那一屏并返回它；找不到则不弹，返回当前屏。
 *（按屏的文件名/display 名匹配，见 VuaScreen 名。）
 */
VuaScreen *vua_session_back_to(VuaSession *session, const char *name);

/* 会话级全局变量（跨屏共享），复用 VusDict。 */
VusDict *vua_session_globals(VuaSession *session);
void     vua_session_global_set(VuaSession *session, VusString *key, void *val);
void    *vua_session_global_get(VuaSession *session, VusString *key);

/*
 * 重绘钩子：屏栈变化（界面_显示 / 界面_返回 / 界面_返回至）引起当前屏变化时被调，
 * 供 APK 的 JNI 层接入，回 Java 触发 View 重建（native → Java 回调）。
 * userdata 原样透传给回调，生命周期由注册方管理。
 */
typedef void (*VuaRerenderHook)(VuaSession *session, void *userdata);
void vua_session_set_rerender_hook(VuaSession *session, VuaRerenderHook hook, void *userdata);

/* ============ 屏幕 / 界面句柄 ============ */

/* 一屏的运行态：组件树 + 变量状态 + 事件表，以及它的屏名。内部 opaque。 */
typedef struct VuaScreen VuaScreen;

/*
 * 从 .vua 的 JSON 源码加载一屏（不入栈；由 vua_session_show 调用）。
 * 内部：yyjson 解析 → 严格校验（type 在控件表、属性在词典）→ 建组件树。
 * 失败返回 NULL 并把首个错误挂到 *err（可为 NULL）。
 */
VuaScreen *vua_screen_load(const char *vua_json, VuaError *err);

/* 释放一屏（含组件树、变量状态、事件表的闭包引用）。 */
void vua_screen_free(VuaScreen *screen);

/* 屏名（通常为 .vua 文件名），供 vua_session_back_to 匹配；无返回 NULL。 */
const char *vua_screen_name(VuaScreen *screen);

/* 屏序号：push 时唯一分配，用于 View diff 判断"是否还是同一屏"。 */
uint64_t vua_screen_seq(VuaScreen *screen);

/* ============ 规范化渲染树（native → Java） ============ */

/*
 * 把解析好的组件树序列化为"规范化渲染树"JSON 字符串（screen 侧缓存所有，
 * 调用方不得 free；见 dump 实现内的 render_cache）。状态变化后自动重建。
 */
const char *vua_screen_dump_rendertree(VuaScreen *screen);

/* 已缓存渲染树字节长度（未构建先构建）。JNI 侧用其分配 byte[]，免 strlen 整树扫描。 */
int vua_screen_rendertree_len(VuaScreen *screen);

/* 取渲染树 JSON 的稳定字节长度（供 JNI NewStringUTF 使用前走长度）。 */
int vua_screen_dump_rendertree_len(const char *rendertree_json);

/*
 * 渲染树内容指纹（64 位 FNV-1a）：供版本号协议用——Java 只凭指纹决定要不要
 * 重建/取 JSON。指纹在 dump 重建缓存时顺带计算；无缓存时先触发 dump。无屏返回 0。
 */
uint64_t vua_screen_rendertree_hash(VuaScreen *screen);

/* ============ 变量状态（复用 VusDict） ============ */

/* 整屏的「变量 → 值」状态表（内部为 VusDict*）。只读获取，修改走 set。 */
VusDict *vua_state(VuaScreen *screen);

/* 写入 变量 = 值。var 为变量名，val 为 VUA 值（VusString* 或 VusObject*）。 */
void vua_state_set(VuaScreen *screen, VusString *var, void *val);

/* 高频路径：变量名直接给 C 串（走字符串驻留，避免每次重建 VusString 键）。 */
void vua_state_set_cstr(VuaScreen *screen, const char *var, void *val);
VusString *vua_state_get_or_empty_cstr(VuaScreen *screen, const char *var);

/* 读取 变量的当前值；变量不存在返回 NULL。 */
void *vua_state_get(VuaScreen *screen, VusString *var);

/* 读取 变量的当前值（安全版）：变量不存在时返回新空字符串（供 .vus 直接消费，避免 NULL）。 */
VusString *vua_state_get_or_empty(VuaScreen *screen, VusString *var);

/* ============ 事件绑定（native / .vus 侧登记） ============ */

/*
 * 把「事件名 → 处理函数」登记进事件表。handler 是 .vus 编译出的处理函数
 * （以 VusClosure 值传入）。同一事件名重复登记覆盖旧值。
 * 成功返回 0，失败（如事件名为空）返回 -1。
 */
int vua_on(VuaScreen *screen, const char *event_name, VusClosure *handler);

/* 解绑事件名（未登记则无操作）。 */
void vua_off(VuaScreen *screen, const char *event_name);

/* ============ 事件派发（Java 触摸回传后调用） ============ */

/*
 * 以事件名派发：查事件表 → 调对应处理函数闭包，arg dict 携带回调变量取值。
 * 未登记的 name 报出 VUA_ERR（打印/抛给上层），不静默。vars 可为 NULL（无参数）。
 */
void vua_trigger_event(VuaScreen *screen, const char *event_name, VusDict *vars);

/*
 * 以控件 id 派发：先到树顶层 eventIndex 把 node_id 解析为 (name, collect)，
 * 再按 name 派发。查不到 id 报错（不静默）。供 Java 的 triggerId 路径使用。
 */
void vua_trigger_by_id(VuaScreen *screen, const char *node_id, VusDict *vars);

/* ============ 控件表 / 词典（供严格校验） ============ */

/*
 * 加载 控件表（中文 type → .so + 可选字段词典）与 全局词典，供 vua_screen_load
 * 校验用。可在任何 vua_screen_load 之前调用一次；未加载则仅做基本 JSON/结构校验。
 */
int vua_control_table_load(const char *control_table_json, VuaError *err);
int vua_dict_load(const char *dict_json, VuaError *err);

/* ============ Makefile / Android.mk 聚合用的登记函数 ============ */

/* libvus_rt 约定：用一个函数返回本模块的全部内建运行时函数，便于生成器映射。 */
/* （草案占位：后续由 generator 的中文名 → C 函数映射表接入。） */
int vua_rt_init(void);
void vua_rt_shutdown(void);

/* ============ JNI / 单一会话辅助 ============ */

/*
 * 模块全局会话（单个 APK 一个）。首次调用创建并缓存。供 JNI 桥直接使用
 * （JNI 是静态的，无 per-context 参数）。失败返回 NULL 并挂 *err。
 */
VuaSession *vua_global_session(VuaError *err);

/*
 * 把 {"变量名":"值", ...} 的 JSON 解析成一个新的 VusDict（值统一为字符串）。
 * 供 JNI 把触摸回传的变量 JSON 转成 native 变量字典。失败返回 NULL 并挂 *err。
 */
VusDict *vua_dict_from_json(const char *vars_json, VuaError *err);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUA_H */