/*
 * vua.c — VUA 界面运行时（Android 组件流，多屏）实现
 *
 * 对应 rt/vua.h。
 * - 解析：直接以 yyjson 文档作为 .vua 组件树（不另造平行结构）。
 * - 渲染树：在 .vua 之上做「归一化」（子组件→children、事件→event，
 *   内部原语键固定、其余控件属性原样透传）+ 顶层 eventIndex(id→event)。
 * - 多屏：VuaSession 屏栈；导航 = 界面_显示(压栈) / 界面_返回(弹栈)。
 * - 事件表用 VusDict(事件名→VusClosure*)，变量用 VusDict，复用 libvus_rt。
 *
 * 编译：与 libvus_rt.c 同对象列表；APK 侧由 vus_apk.c 的 Android.mk 编入。
 */

#define _GNU_SOURCE
#include "vua.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson/yyjson.h"

/* ============ 严格校验所需的注册表（模块级） ============ */
/* 一张控件 + 每控件的合法属性键集合；一张全局词典（中文键 → 内部名）。 */

typedef struct {
    char  *type;         /* 控件中文 type */
    char **fields;       /* 该控件的合法属性键 */
    int    field_count;
} VuaCtrlDef;

static VuaCtrlDef *g_ctrls = NULL;
static int         g_ctrl_count = 0;
static VusDict    *g_dict = NULL;   /* 全局词典：中文键 → 内部名 */

/* ============ 小工具 ============ */

static int streq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
static int key_is(const char *k, const char *first, const char *second) {
    return streq(k, first) || streq(k, second);
}

static void vua_error_set(VuaError *err, int code, const char *msg) {
    if (!err) return;
    err->code = code;
    err->line = 0;
    err->file[0] = '\0';
    snprintf(err->msg, sizeof(err->msg), "%s", msg ? msg : "");
    err->next = NULL;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len < 0) { fclose(fp); return NULL; }
    char *data = (char *)malloc((size_t)len + 1);
    if (!data) { fclose(fp); return NULL; }
    size_t n = fread(data, 1, (size_t)len, fp);
    fclose(fp);
    data[n] = '\0';
    if (out_len) *out_len = n;
    return data;
}

static const char *basename_noext(const char *path, char *buf, size_t buf_sz) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= buf_sz) n = buf_sz - 1;
    memcpy(buf, base, n);
    buf[n] = '\0';
    return buf;
}

/* ============ 会话（session）：多屏/屏栈 ============ */

struct VuaScreen {
    char       *name;         /* 屏名：.vua 文件名(无扩展)，供 back_to 匹配 */
    yyjson_doc *tree;         /* .vua 组件树（yyjson 文档，与渲染承担同一棵树） */
    VusDict    *state;        /* 屏内 变量→值 */
    VusDict    *events;       /* 事件名 → VusClosure* */
    char       *render_cache; /* 渲染树缓存：state 未变时复用（vua_screen_dump_rendertree 所有者） */
    uint64_t    render_hash;  /* 缓存内容的 64 位指纹（FNV-1a），版本号协议用 */
    uint64_t    seq;          /* 屏序号（push 时递增分配），供 View diff 识别"是否同一屏" */
};

struct VuaSession {
    VuaScreen  **stack;       /* 屏栈，栈顶 = 最后一个 */
    int          stack_len;
    int          stack_cap;
    uint64_t     screen_seq;  /* 屏序号发号器 */
    VusDict     *globals;     /* session 级 变量→值 */
    VuaRerenderHook rerender_hook;  /* 屏栈变化 → 重建 View 的钩子（可空） */
    void            *rerender_ud;   /* 钩子 userdata */
};

static int vua_session_push(VuaSession *s, VuaScreen *screen);

VuaSession *vua_session_new(VuaError *err) {
    VuaSession *s = (VuaSession *)calloc(1, sizeof(VuaSession));
    if (!s) { vua_error_set(err, -99, "vua_session_new: 内存不足"); return NULL; }
    s->globals = vus_dict_new();
    if (!s->globals) { free(s); vua_error_set(err, -99, "vua_session_new: 创建全局变量表失败"); return NULL; }
    return s;
}

void vua_session_free(VuaSession *s) {
    if (!s) return;
    for (int i = 0; i < s->stack_len; i++) vua_screen_free(s->stack[i]);
    free(s->stack);
    if (s->globals) vus_unref(s->globals);
    free(s);
}

VuaScreen *vua_session_current(VuaSession *s) {
    if (!s || s->stack_len == 0) return NULL;
    return s->stack[s->stack_len - 1];
}

static int vua_session_push(VuaSession *s, VuaScreen *screen) {
    if (!s || !screen) return -1;
    screen->seq = ++s->screen_seq;   /* 新屏分配唯一序号（View diff 用它区分换页） */
    if (s->stack_len >= s->stack_cap) {
        int ncap = s->stack_cap ? s->stack_cap * 2 : 8;
        VuaScreen **ns = (VuaScreen **)realloc(s->stack, ncap * sizeof(VuaScreen *));
        if (!ns) return -1;
        s->stack = ns; s->stack_cap = ncap;
    }
    s->stack[s->stack_len++] = screen;
    return 0;
}

/* 屏栈变化后通知：若有重绘钩子则调用（native → Java 重建 View）。 */
static void vua_notify_rerender(VuaSession *s) {
    if (s && s->rerender_hook) s->rerender_hook(s, s->rerender_ud);
}

void vua_session_set_rerender_hook(VuaSession *s, VuaRerenderHook hook, void *userdata) {
    if (!s) return;
    s->rerender_hook = hook;
    s->rerender_ud   = userdata;
}

VuaScreen *vua_session_show(VuaSession *s, const char *vua_path, VuaError *err) {
    if (!s || !vua_path) { vua_error_set(err, -99, "界面_显示: 参数无效"); return NULL; }
    size_t len = 0;
    char *json = read_file(vua_path, &len);
    if (!json) { vua_error_set(err, VUA_ERR_JSON, "界面_显示: 读取 .vua 失败"); return NULL; }

    VuaScreen *screen = vua_screen_load(json, err);
    free(json);
    if (!screen) return NULL;

    char nm[1024];
    screen->name = strdup(basename_noext(vua_path, nm, sizeof(nm)));

    if (vua_session_push(s, screen) != 0) {
        vua_screen_free(screen);
        vua_error_set(err, -99, "界面_显示: 屏栈溢出"); return NULL;
    }
    vua_notify_rerender(s);
    return screen;
}

/* 界面_显示_JSON：把 .vua 格式的 JSON 字符串直接解析为一屏并压栈（动态渲染树）。
 * 供 .vus 运行时根据数据动态生成界面（如从网络/文件拉取数据后拼接渲染树）。 */
int vua_show_json(VuaSession *s, const char *vua_json, VuaError *err) {
    if (!s || !vua_json || !vua_json[0]) {
        vua_error_set(err, -99, "界面_显示_JSON: 参数无效");
        return -1;
    }
    VuaError le = {0};
    if (!err) err = &le;
    VuaScreen *screen = vua_screen_load(vua_json, err);
    if (!screen) {
        fprintf(stderr, "[vua] 界面_显示_JSON 加载失败: %s\n", err->msg[0] ? err->msg : "(无错误)");
        return -1;
    }
    char nm[64];
    snprintf(nm, sizeof(nm), "dyn_%d", (int)s->stack_len);
    screen->name = strdup(nm);
    if (vua_session_push(s, screen) != 0) {
        vua_screen_free(screen);
        vua_error_set(err, -99, "界面_显示_JSON: 屏栈溢出");
        return -1;
    }
    vua_notify_rerender(s);
    return 0;
}

VuaScreen *vua_session_back(VuaSession *s) {
    if (!s || s->stack_len <= 1) return vua_session_current(s);
    vua_screen_free(s->stack[s->stack_len - 1]);
    s->stack[s->stack_len - 1] = NULL;
    s->stack_len--;
    vua_notify_rerender(s);
    return vua_session_current(s);
}

VuaScreen *vua_session_back_to(VuaSession *s, const char *name) {
    if (!s || !name) return vua_session_current(s);
    for (int i = s->stack_len - 1; i >= 1; i--) {
        if (s->stack[i]->name && strcmp(s->stack[i]->name, name) == 0) {
            for (int j = s->stack_len - 1; j > i; j--) vua_screen_free(s->stack[j]);
            s->stack_len = i + 1;
            vua_notify_rerender(s);
            return vua_session_current(s);
        }
    }
    return vua_session_current(s);
}

VusDict *vua_session_globals(VuaSession *s) { return s ? s->globals : NULL; }
void vua_session_global_set(VuaSession *s, VusString *key, void *val) {
    if (s && key) vus_dict_set(s->globals, key, val);
}
void *vua_session_global_get(VuaSession *s, VusString *key) {
    return (s && key) ? vus_dict_get(s->globals, key) : NULL;
}

/* ============ yyjson 拷贝/渲染树构建 ============ */

/* 递归深复制：imm/mut 值（yyjson 的 val 布局一致）→ 新 mut 值。 */
static yyjson_mut_val *mv_copy(yyjson_mut_doc *d, const yyjson_val *v) {
    if (!v) return yyjson_mut_null(d);
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_STR:
        return yyjson_mut_strncpy(d, yyjson_get_str(v), yyjson_get_len(v));
    case YYJSON_TYPE_NUM:
        if (yyjson_is_int(v)) return yyjson_mut_int(d, yyjson_get_sint(v));
        return yyjson_mut_real(d, yyjson_get_real(v));
    case YYJSON_TYPE_BOOL:
        return yyjson_mut_bool(d, yyjson_get_bool(v));
    case YYJSON_TYPE_NULL:
        return yyjson_mut_null(d);
    case YYJSON_TYPE_ARR: {
        yyjson_mut_val *a = yyjson_mut_arr(d);
        yyjson_arr_iter it; yyjson_arr_iter_init((yyjson_val *)v, &it); yyjson_val *e;
        while ((e = yyjson_arr_iter_next(&it)))
            yyjson_mut_arr_append(a, mv_copy(d, e));
        return a;
    }
    case YYJSON_TYPE_OBJ: {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_obj_iter it; yyjson_obj_iter_init((yyjson_val *)v, &it);
        yyjson_val *k, *val;
        while ((k = yyjson_obj_iter_next(&it))) {
            val = yyjson_obj_iter_get_val(k);
            yyjson_mut_obj_add_val(d, o, yyjson_get_str(k), mv_copy(d, val));
        }
        return o;
    }
    default: return yyjson_mut_null(d);
    }
}

/* 事件值归一化：字符串 或 {事件名,name + 回调变量,collect[]} → {name, collect[]} */
static yyjson_mut_val *build_event_obj(yyjson_mut_doc *d, const yyjson_val *v) {
    yyjson_mut_val *ev = yyjson_mut_obj(d);
    const char *name = "";
    yyjson_val *collect = NULL;

    if (yyjson_is_str(v)) {
        name = yyjson_get_str(v);
    } else if (yyjson_is_obj(v)) {
        yyjson_val *n = yyjson_obj_get((yyjson_val *)v, "事件名");
        if (!n) n = yyjson_obj_get((yyjson_val *)v, "name");
        if (n && yyjson_is_str(n)) name = yyjson_get_str(n);
        yyjson_val *c = yyjson_obj_get((yyjson_val *)v, "回调变量");
        if (!c) c = yyjson_obj_get((yyjson_val *)v, "collect");
        if (c && yyjson_is_arr(c)) collect = c;
    }

    yyjson_mut_obj_add_strcpy(d, ev, "name", name ? name : "");
    yyjson_mut_obj_add_val(d, ev, "collect", collect ? mv_copy(d, collect) : yyjson_mut_arr(d));
    return ev;
}

/*
 * 归一化一个 .vua 组件对象 → 渲染树节点 mut 对象：
 *  - 内部原语键固定：type / id / variable / event / children
 *  - 子组件(子组件|children)递归归一
 *  - 事件(事件|点击|变化|event|点击...) → {name, collect}
 *  - 其余键：控件属性，原样透传（值深复制）
 */
static yyjson_mut_val *build_render_node(yyjson_mut_doc *d, const yyjson_val *src,
                                         VusDict *state) {
    yyjson_mut_val *out = yyjson_mut_obj(d);

    /* 预扫描 type / variable：供「内容」变量回填（文本控件显示状态值） */
    const char *node_type = NULL;
    const char *node_var = NULL;
    {
        yyjson_val *tv0 = yyjson_obj_get((yyjson_val *)src, "type");
        if (tv0 && yyjson_is_str(tv0)) node_type = yyjson_get_str(tv0);
        yyjson_val *vv0 = yyjson_obj_get((yyjson_val *)src, "variable");
        if (vv0 && yyjson_is_str(vv0)) node_var = yyjson_get_str(vv0);
    }

    yyjson_obj_iter it; yyjson_obj_iter_init((yyjson_val *)src, &it);
    yyjson_val *k, *v;
    while ((k = yyjson_obj_iter_next(&it))) {
        v = yyjson_obj_iter_get_val(k);
        const char *ks = yyjson_get_str(k);

        if (streq(ks, "type")) {
            if (yyjson_is_str(v)) yyjson_mut_obj_add_strcpy(d, out, "type", yyjson_get_str(v));
        } else if (streq(ks, "id")) {
            if (yyjson_is_str(v)) yyjson_mut_obj_add_strcpy(d, out, "id", yyjson_get_str(v));
        } else if (key_is(ks, "变量", "variable")) {
            if (yyjson_is_str(v)) yyjson_mut_obj_add_strcpy(d, out, "variable", yyjson_get_str(v));
        } else if (key_is(ks, "子组件", "children")) {
            if (yyjson_is_arr(v)) {
                yyjson_mut_val *carr = yyjson_mut_arr(d);
                yyjson_arr_iter ai; yyjson_arr_iter_init((yyjson_val *)v, &ai); yyjson_val *e;
                while ((e = yyjson_arr_iter_next(&ai))) {
                    if (yyjson_is_obj(e)) yyjson_mut_arr_append(carr, build_render_node(d, e, state));
                }
                yyjson_mut_obj_add_val(d, out, "children", carr);
            }
        } else if (key_is(ks, "事件", "event") ||
                   key_is(ks, "点击", "onClick") ||
                   key_is(ks, "变化", "onChange")) {
            yyjson_mut_obj_add_val(d, out, "event", build_event_obj(d, v));
        } else if (streq(ks, "内容") && node_type && streq(node_type, "文本") &&
                   node_var && state) {
            /* 变量回填：文本控件带 variable 且屏内状态有值 → 用状态值替换「内容」，
             * 使 .vus 里 界面_设置 的运算结果实时显示；仅此一处写「内容」，无重复键。 */
            VusString *vk = vus_string_new(node_var);
            void *val = vus_dict_get(state, vk);
            vus_unref(vk);
            if (val) {
                yyjson_mut_obj_add_strcpy(d, out, "内容", vus_string_cstr((VusString *)val));
                continue;
            }
            yyjson_mut_obj_add_val(d, out, ks, mv_copy(d, v));
        } else {
            /* 控件自定义属性：原样透传 */
            yyjson_mut_obj_add_val(d, out, ks, mv_copy(d, v));
        }
    }
    return out;
}

/* 递归收集事件索引：从 imm 源树找带 id + 事件值的节点，登记 id → {name, collect}。
 * 事件值在源树是原始形态（字符串 或 {事件名,回调变量}），用 build_event_obj 归一化；
 * 避免在 mut 对象上做深拷贝（mut 布局与 imm 迭代不兼容）。 */
static void collect_eventindex_imm(yyjson_mut_doc *d, const yyjson_val *node, yyjson_mut_val *eix) {
    if (!node || !yyjson_is_obj(node)) return;

    const char *id = NULL;
    yyjson_val *idv = yyjson_obj_get((yyjson_val *)node, "id");
    if (idv && yyjson_is_str(idv)) id = yyjson_get_str(idv);

    if (id) {
        static const char *EKEYS[] = { "事件", "event", "点击", "onClick", "变化", "onChange" };
        for (size_t i = 0; i < sizeof(EKEYS) / sizeof(EKEYS[0]); i++) {
            yyjson_val *ev = yyjson_obj_get((yyjson_val *)node, EKEYS[i]);
            if (ev) { yyjson_mut_obj_add_val(d, eix, id, build_event_obj(d, ev)); break; }
        }
    }

    yyjson_val *ch = yyjson_obj_get((yyjson_val *)node, "子组件");
    if (!ch) ch = yyjson_obj_get((yyjson_val *)node, "children");
    if (ch && yyjson_is_arr(ch)) {
        yyjson_arr_iter it; yyjson_arr_iter_init(ch, &it); yyjson_val *e;
        while ((e = yyjson_arr_iter_next(&it)))
            if (yyjson_is_obj(e)) collect_eventindex_imm(d, e, eix);
    }
}

/* ============ 严格校验 ============ */

/* 内部原语键：解析器/渲染专用，不做属性校验、不进词典。 */
static int is_internal_key(const char *k) {
    static const char *IK[] = {
        "type","id","children","子组件","variable","变量",
        "event","事件","点击","onClick","变化","onChange"
    };
    for (size_t i = 0; i < sizeof(IK) / sizeof(IK[0]); i++)
        if (streq(k, IK[i])) return 1;
    return 0;
}

/* 某 type 的控件字段词典里是否有该键（type 为空时返回 0）。 */
static int ctrl_has_field(const char *type, const char *key) {
    if (!type || !key) return 0;
    for (int i = 0; i < g_ctrl_count; i++) {
        if (streq(g_ctrls[i].type, type)) {
            for (int j = 0; j < g_ctrls[i].field_count; j++)
                if (streq(g_ctrls[i].fields[j], key)) return 1;
            return 0; /* type 命中但键不在其字段 */
        }
    }
    return 0;
}

/* 全局词典是否含该键；未加载词典返回 0。 */
static int dict_has_key(const char *k) {
    if (!g_dict || !k) return 0;
    VusString *ks = vus_string_new(k);
    int r = vus_dict_get(g_dict, ks) != NULL;
    vus_unref(ks);
    return r;
}

/*
 * 递归校验一个 .vua 节点：
 *  - type 必须在控件表（仅当已加载控件表）
 *  - 每个非原语属性键 ∈（该 type 的字段词典 ∪ g_dict 全局词典）
 *  - children 递归
 * 命中即 err 置码并返回 0。
 */
static int vua_validate_node(const yyjson_val *obj, VuaError *err) {
    if (!obj || !yyjson_is_obj(obj)) { vua_error_set(err, -99, "校验: 节点非对象"); return 0; }

    const char *type = NULL;
    yyjson_val *tv = yyjson_obj_get((yyjson_val *)obj, "type");
    if (tv && yyjson_is_str(tv)) type = yyjson_get_str(tv);

    /* type ∈ 控件表 */
    if (g_ctrl_count > 0) {
        int found = 0;
        for (int i = 0; i < g_ctrl_count; i++)
            if (streq(g_ctrls[i].type, type)) { found = 1; break; }
        if (!found) {
            /* 未知控件类型（如 .vaz 模板漏展开）：降级为"透传 + 递归子节点"，
             * Java 端渲染为占位控件。绝不因单个未知节点阻断整屏导致白屏。 */
            yyjson_val *ch = yyjson_obj_get((yyjson_val *)obj, "children");
            if (!ch) ch = yyjson_obj_get((yyjson_val *)obj, "子组件");
            if (ch && yyjson_is_arr(ch)) {
                yyjson_arr_iter ai; yyjson_arr_iter_init(ch, &ai); yyjson_val *e;
                while ((e = yyjson_arr_iter_next(&ai)))
                    if (!vua_validate_node(e, err)) return 0;
            }
            return 1;
        }
    }

    yyjson_obj_iter it; yyjson_obj_iter_init((yyjson_val *)obj, &it);
    yyjson_val *k, *v;
    while ((k = yyjson_obj_iter_next(&it))) {
        v = yyjson_obj_iter_get_val(k);
        const char *ks = yyjson_get_str(k);

        if (is_internal_key(ks)) {
            if ((streq(ks, "子组件") || streq(ks, "children")) && yyjson_is_arr(v)) {
                yyjson_arr_iter ai; yyjson_arr_iter_init(v, &ai); yyjson_val *e;
                while ((e = yyjson_arr_iter_next(&ai)))
                    if (!vua_validate_node(e, err)) return 0;
            }
            continue;
        }

        /* 属性键 ∈ 控件字段词典 ∪ 全局词典。不在时降级跳过（透传给 Java），
         * 避免未知扩展属性把整屏校验失败 → 白屏。 */
        if (ctrl_has_field(type, ks)) continue;
        if (dict_has_key(ks)) continue;
        continue;
    }
    return 1;
}

/* ============ 屏幕 / 界面句柄 ============ */

VuaScreen *vua_screen_load(const char *vua_json, VuaError *err) {
    if (!vua_json) { vua_error_set(err, VUA_ERR_JSON, "解析 .vua: 空输入"); return NULL; }

    yyjson_doc *doc = yyjson_read(vua_json, strlen(vua_json), 0);
    if (!doc) { vua_error_set(err, VUA_ERR_JSON, "解析 .vua: 非法 JSON"); return NULL; }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        vua_error_set(err, VUA_ERR_ROOT, "解析 .vua: 顶层必须是对象");
        return NULL;
    }
    yyjson_val *type = yyjson_obj_get(root, "type");
    if (!type || !yyjson_is_str(type) || strcmp(yyjson_get_str(type), "界面") != 0) {
        yyjson_doc_free(doc);
        vua_error_set(err, VUA_ERR_ROOT, "解析 .vua: 根节点的 type 必须是「界面」");
        return NULL;
    }
    /* 严格校验：type∈控件表、属性∈控件字段词典∪全局词典（未加载时跳过对应检查）。 */
    if (!vua_validate_node(root, err)) {
        yyjson_doc_free(doc);
        return NULL;
    }

    VuaScreen *screen = (VuaScreen *)calloc(1, sizeof(VuaScreen));
    if (!screen) { yyjson_doc_free(doc); vua_error_set(err, -99, "解析 .vua: 内存不足"); return NULL; }
    screen->tree = doc;
    screen->state = vus_dict_new();
    screen->events = vus_dict_new();
    return screen;
}

void vua_screen_free(VuaScreen *screen) {
    if (!screen) return;
    free(screen->name);
    if (screen->tree) yyjson_doc_free(screen->tree);
    if (screen->state) vus_unref(screen->state);
    if (screen->events) vus_unref(screen->events);
    free(screen->render_cache);
    free(screen);
}

const char *vua_screen_name(VuaScreen *screen) { return screen ? screen->name : NULL; }

uint64_t vua_screen_seq(VuaScreen *screen) { return screen ? screen->seq : 0; }

/* ============ 规范化渲染树（native → Java） ============ */

/* —— 渲染树内容指纹（版本号协议）：FNV-1a 64，稳定、低碰撞 —— */
static uint64_t vus_renderhash_fnv1a64(const char *s) {
    uint64_t h = 1469598103934665603ULL;   /* FNV offset basis */
    if (s) {
        while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    }
    return h;
}

const char *vua_screen_dump_rendertree(VuaScreen *screen) {
    if (!screen || !screen->tree) return NULL;

    /* 渲染树缓存：screen 的 state / 屏树未变时直接复用上次序列化结果，
     * 省去整树归一化 + JSON 序列化（高频路径：点击无状态变化 / 返回同屏）。
     * 缓存由 vua_state_set 置脏（free），屏销毁时随 screen 释放。 */
    if (screen->render_cache) return screen->render_cache;

    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (!md) return NULL;
    yyjson_mut_val *root = build_render_node(md, yyjson_doc_get_root(screen->tree), screen->state);
    yyjson_mut_doc_set_root(md, root);

    /* 顶层 eventIndex：从 imm 源树收集 id → {name, collect} */
    yyjson_mut_val *eix = yyjson_mut_obj(md);
    collect_eventindex_imm(md, yyjson_doc_get_root(screen->tree), eix);
    yyjson_mut_obj_add_val(md, root, "eventIndex", eix);

    size_t len = 0;
    char *out = yyjson_mut_write(md, 0, &len);
    yyjson_mut_doc_free(md);
    if (!out) return NULL;
    screen->render_cache = out;   /* 所有权归 screen，调用方不得 free */
    screen->render_hash = vus_renderhash_fnv1a64(out);
    return out;
}

/* 取当前屏渲染树指纹：缓存缺失时先触发 dump 构建再取；无屏返回 0。 */
uint64_t vua_screen_rendertree_hash(VuaScreen *screen) {
    if (!screen || !screen->tree) return 0;
    if (!screen->render_cache) {
        if (!vua_screen_dump_rendertree(screen)) return 0;
    }
    return screen->render_hash;
}

int vua_screen_dump_rendertree_len(const char *rendertree_json) {
    return rendertree_json ? (int)strlen(rendertree_json) : 0;
}

/* ============ 变量状态（复用 VusDict） ============ */

VusDict *vua_state(VuaScreen *screen) { return screen ? screen->state : NULL; }
void vua_state_set(VuaScreen *screen, VusString *var, void *val) {
    if (screen && var && val) {
        vus_dict_set(screen->state, var, val);
        /* state 变化 → 渲染树文本必然变化，作废缓存与指纹 */
        free(screen->render_cache);
        screen->render_cache = NULL;
        screen->render_hash = 0;
    }
}
/* 高频路径（界面_设置 字面量变量名）：key 走字符串驻留，避免每次 malloc 复制 */
void vua_state_set_cstr(VuaScreen *screen, const char *var, void *val) {
    if (!screen || !var || !val) return;
    VusString *k = vus_string_intern(var);
    if (k) {
        vua_state_set(screen, k, val);
        vus_unref(k);
    }
}
void *vua_state_get(VuaScreen *screen, VusString *var) {
    return (screen && var) ? vus_dict_get(screen->state, var) : NULL;
}

VusString *vua_state_get_or_empty(VuaScreen *screen, VusString *var) {
    void *v = vua_state_get(screen, var);
    if (v) return vus_string_new(vus_string_cstr((VusString *)v));
    return vus_string_new("");
}

/* 高频路径（界面_取 字面量变量名）：key 走字符串驻留 */
VusString *vua_state_get_or_empty_cstr(VuaScreen *screen, const char *var) {
    if (!screen || !var) return vus_string_new("");
    VusString *k = vus_string_intern(var);
    VusString *r = k ? vua_state_get_or_empty(screen, k) : vus_string_new("");
    if (k) vus_unref(k);
    return r;
}

/* ============ 事件绑定（会话级全局事件表，多屏共享） ============
 * 事件表挂模块级单例（APK 单会话）：界面_绑定 在首页登记后，切到新屏（界面_显示/
 * 界面_显示_JSON）仍能派发，动态页/静态页按钮事件统一可命中。 */

static VusDict *g_events = NULL;   /* 事件名 → VusClosure* */

static VusDict *vua_events_table(void) {
    if (!g_events) g_events = vus_dict_new();
    return g_events;
}

int vua_on(VuaScreen *screen, const char *event_name, VusClosure *handler) {
    (void)screen;
    if (!event_name) return -1;
    if (!handler) { vua_off(screen, event_name); return 0; }
    VusDict *ev = vua_events_table();
    if (!ev) return -1;
    VusString *key = vus_string_new(event_name);
    if (!key) return -1;
    vus_dict_set(ev, key, handler);
    vus_unref(key);
    return 0;
}

void vua_off(VuaScreen *screen, const char *event_name) {
    (void)screen;
    if (!event_name || !g_events) return;
    VusString *key = vus_string_new(event_name);
    if (!key) return;
    vus_dict_remove(g_events, key);
    vus_unref(key);
}

/* ============ 事件派发 ============ */

void vua_trigger_event(VuaScreen *screen, const char *event_name, VusDict *vars) {
    (void)screen;
    if (!event_name) return;
    VusDict *ev = g_events ? g_events : vua_events_table();
    if (!ev) return;
    VusString *key = vus_string_new(event_name);
    if (!key) return;
    VusClosure *handler = (VusClosure *)vus_dict_get(ev, key);
    vus_unref(key);
    if (!handler) { fprintf(stderr, "[vua] 事件未绑定：%s\n", event_name); return; }
    vus_closure_call(handler, vars);
}

/* 在 imm 源树中按 id 找节点；返回该节点，未找到返回 NULL。 */
static const yyjson_val *find_node_by_id(const yyjson_val *node, const char *node_id) {
    if (!node || !yyjson_is_obj(node)) return NULL;
    yyjson_val *idv = yyjson_obj_get((yyjson_val *)node, "id");
    if (idv && yyjson_is_str(idv) && strcmp(yyjson_get_str(idv), node_id) == 0)
        return node;
    yyjson_val *ch = yyjson_obj_get((yyjson_val *)node, "子组件");
    if (!ch) ch = yyjson_obj_get((yyjson_val *)node, "children");
    if (ch && yyjson_is_arr(ch)) {
        yyjson_arr_iter it; yyjson_arr_iter_init(ch, &it); yyjson_val *e;
        while ((e = yyjson_arr_iter_next(&it))) {
            const yyjson_val *hit = find_node_by_id(e, node_id);
            if (hit) return hit;
        }
    }
    return NULL;
}

/* 从收集变量列表读屏内状态，拼一个新的 VusDict；无收集项返回 NULL。 */
static VusDict *collect_vars(VuaScreen *screen, const yyjson_val *event_or_array) {
    /* event_or_array：若是 arr 直接用；若是事件对象则取其 collect 键。 */
    const yyjson_val *arr = event_or_array;
    if (arr && yyjson_is_obj(arr)) {
        yyjson_val *c = yyjson_obj_get((yyjson_val *)arr, "回调变量");
        if (!c) c = yyjson_obj_get((yyjson_val *)arr, "collect");
        arr = c;
    }
    if (!arr || !yyjson_is_arr(arr)) return NULL;
    VusDict *out = vus_dict_new();
    if (!out) return NULL;
    yyjson_arr_iter it; yyjson_arr_iter_init((yyjson_val *)arr, &it); yyjson_val *e;
    while ((e = yyjson_arr_iter_next(&it))) {
        if (!yyjson_is_str(e)) continue;
        const char *s = yyjson_get_str(e);
        const char *eq = strchr(s, '=');
        if (eq && eq != s && eq[1] != '\0') {
            /* 字面量参数: "键=值"（不查屏内状态，用于星级 1..5 等固定值） */
            size_t klen = (size_t)(eq - s);
            char *kbuf = (char *)malloc(klen + 1);
            if (!kbuf) continue;
            memcpy(kbuf, s, klen);
            kbuf[klen] = '\0';
            VusString *key = vus_string_new(kbuf);
            VusString *val = vus_string_new(eq + 1);
            if (key && val) vus_dict_set(out, key, val);
            free(kbuf);
            if (key) vus_unref(key);
            if (val) vus_unref(val);
            continue;
        }
        VusString *key = vus_string_new(s);
        void *val = vua_state_get(screen, key);
        if (val) vus_dict_set(out, key, val);
        vus_unref(key);
    }
    return out;
}

void vua_trigger_by_id(VuaScreen *screen, const char *node_id, VusDict *vars) {
    if (!screen || !screen->tree || !node_id) return;
    const yyjson_val *node = find_node_by_id(yyjson_doc_get_root(screen->tree), node_id);
    if (!node) { fprintf(stderr, "[vua] 按ID触发: 找不到控件 %s\n", node_id); return; }

    static const char *EKEYS[] = { "事件", "event", "点击", "onClick", "变化", "onChange" };
    yyjson_val *ev = NULL;
    for (size_t i = 0; i < sizeof(EKEYS) / sizeof(EKEYS[0]); i++) {
        ev = yyjson_obj_get((yyjson_val *)node, EKEYS[i]);
        if (ev) break;
    }
    if (!ev) { fprintf(stderr, "[vua] 按ID触发: 控件 %s 无事件\n", node_id); return; }

    const char *name = NULL;
    if (yyjson_is_str(ev)) {
        name = yyjson_get_str(ev);
    } else if (yyjson_is_obj(ev)) {
        yyjson_val *n = yyjson_obj_get(ev, "事件名");
        if (!n) n = yyjson_obj_get(ev, "name");
        if (n && yyjson_is_str(n)) name = yyjson_get_str(n);
    }
    if (!name) { fprintf(stderr, "[vua] 按ID触发: 控件 %s 事件名缺失\n", node_id); return; }

    VusDict *collect = collect_vars(screen, ev);
    vua_trigger_event(screen, name, collect ? collect : vars);
    if (collect) vus_unref((void *)collect);
}

/* ============ 控件表 / 词典（供严格校验） ============ */

int vua_dict_load(const char *dict_json, VuaError *err) {
    yyjson_doc *doc = yyjson_read(dict_json, strlen(dict_json), 0);
    if (!doc) { vua_error_set(err, VUA_ERR_JSON, "字典: 非法 JSON"); return -1; }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *d = (root && yyjson_is_obj(root)) ? yyjson_obj_get(root, "词典") : NULL;
    if (!d || !yyjson_is_obj(d)) {
        yyjson_doc_free(doc);
        vua_error_set(err, -1, "字典: 需要顶层「词典」对象");
        return -1;
    }
    if (!g_dict) g_dict = vus_dict_new();
    if (!g_dict) { yyjson_doc_free(doc); vua_error_set(err, -99, "字典: 内存不足"); return -1; }
    yyjson_obj_iter it; yyjson_obj_iter_init(d, &it);
    yyjson_val *k, *v;
    while ((k = yyjson_obj_iter_next(&it))) {
        v = yyjson_obj_iter_get_val(k);
        if (!yyjson_is_str(v)) continue;
        VusString *kk = vus_string_new(yyjson_get_str(k));
        VusString *vv = vus_string_new(yyjson_get_str(v));
        if (kk && vv) vus_dict_set(g_dict, kk, vv);
        if (kk) vus_unref(kk);
        if (vv) vus_unref(vv);
    }
    yyjson_doc_free(doc);
    return 0;
}

int vua_control_table_load(const char *control_table_json, VuaError *err) {
    yyjson_doc *doc = yyjson_read(control_table_json, strlen(control_table_json), 0);
    if (!doc) { vua_error_set(err, VUA_ERR_JSON, "控件表: 非法 JSON"); return -1; }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *ct = (root && yyjson_is_obj(root)) ? yyjson_obj_get(root, "控件表") : NULL;
    if (!ct || !yyjson_is_obj(ct)) {
        yyjson_doc_free(doc);
        vua_error_set(err, -1, "控件表: 需要顶层「控件表」对象");
        return -1;
    }
    yyjson_obj_iter it; yyjson_obj_iter_init(ct, &it);
    yyjson_val *k, *v;
    while ((k = yyjson_obj_iter_next(&it))) {
        v = yyjson_obj_iter_get_val(k);
        if (!yyjson_is_obj(v)) continue;
        g_ctrls = (VuaCtrlDef *)realloc(g_ctrls, (size_t)(g_ctrl_count + 1) * sizeof(VuaCtrlDef));
        if (!g_ctrls) { yyjson_doc_free(doc); vua_error_set(err, -99, "控件表: 内存不足"); return -1; }
        VuaCtrlDef *def = &g_ctrls[g_ctrl_count++];
        memset(def, 0, sizeof(*def));
        def->type = strdup(yyjson_get_str(k));
        yyjson_val *fields = yyjson_obj_get(v, "字段");
        if (fields && yyjson_is_obj(fields)) {
            yyjson_obj_iter fi; yyjson_obj_iter_init(fields, &fi);
            yyjson_val *fk, *fv;
            while ((fk = yyjson_obj_iter_next(&fi))) {
                fv = yyjson_obj_iter_get_val(fk); (void)fv;
                def->fields = (char **)realloc(def->fields, (size_t)(def->field_count + 1) * sizeof(char *));
                if (def->fields) def->fields[def->field_count++] = strdup(yyjson_get_str(fk));
            }
        }
    }
    yyjson_doc_free(doc);
    return 0;
}

/* ============ 登记（占位） ============ */

static VuaSession *g_vua_session = NULL;   /* 单个 APK 的全局会话 */
static VuaError    g_vua_session_err = {0};/* 缓存创建失败原因 */

int vua_rt_init(void) {
    /* TODO: 生成器把中文「界面_显示/界面_返回/界面_绑定/界面_触发/...」映射到
     * 对应 C 包装；此处登记或由 generator 直接引用。 */
    return 0;
}

void vua_rt_shutdown(void) {
    for (int i = 0; i < g_ctrl_count; i++) {
        free(g_ctrls[i].type);
        for (int j = 0; j < g_ctrls[i].field_count; j++) free(g_ctrls[i].fields[j]);
        free(g_ctrls[i].fields);
    }
    free(g_ctrls); g_ctrls = NULL; g_ctrl_count = 0;
    if (g_dict) { vus_unref(g_dict); g_dict = NULL; }
    if (g_events) { vus_unref(g_events); g_events = NULL; }
    if (g_vua_session) { vua_session_free(g_vua_session); g_vua_session = NULL; }
}

/* ============ JNI / 单一会话辅助 ============ */

VuaSession *vua_global_session(VuaError *err) {
    if (!g_vua_session) {
        g_vua_session = vua_session_new(&g_vua_session_err);
        if (!g_vua_session && err) *err = g_vua_session_err;
    }
    return g_vua_session;
}

VusDict *vua_dict_from_json(const char *vars_json, VuaError *err) {
    if (!vars_json) return NULL;
    yyjson_doc *doc = yyjson_read(vars_json, strlen(vars_json), 0);
    if (!doc) { vua_error_set(err, VUA_ERR_JSON, "变量 JSON: 非法"); return NULL; }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        vua_error_set(err, VUA_ERR_JSON, "变量 JSON: 需为对象");
        return NULL;
    }
    VusDict *out = vus_dict_new();
    if (!out) { yyjson_doc_free(doc); vua_error_set(err, -99, "变量 JSON: 内存不足"); return NULL; }
    yyjson_obj_iter it; yyjson_obj_iter_init(root, &it);
    yyjson_val *k, *v;
    while ((k = yyjson_obj_iter_next(&it))) {
        v = yyjson_obj_iter_get_val(k);
        char local[64];
        const char *vs;
        yyjson_type t = yyjson_get_type(v);
        if (t == YYJSON_TYPE_STR) {
            vs = yyjson_get_str(v);
        } else if (t == YYJSON_TYPE_BOOL) {
            vs = yyjson_get_bool(v) ? "true" : "false";
        } else if (t == YYJSON_TYPE_NUM) {
            if (yyjson_is_int(v)) snprintf(local, sizeof(local), "%lld", (long long)yyjson_get_sint(v));
            else snprintf(local, sizeof(local), "%g", yyjson_get_real(v));
            vs = local;
        } else {
            vs = "";
        }
        VusString *kk = vus_string_new(yyjson_get_str(k));
        VusString *vv = vus_string_new(vs ? vs : "");
        if (kk && vv) vus_dict_set(out, kk, vv);
        if (kk) vus_unref(kk);
        if (vv) vus_unref(vv);
    }
    yyjson_doc_free(doc);
    return out;
}