/*
 * test_plugin_inproc.c — VUS 进程内插件调用与 JSON 转换的 C 单元测试
 *
 * 覆盖：
 *   1) vus_json_parse：解析 JSON 字符串为结构化 VusObject（字典/列表）
 *   2) vus_json_generate：将结构化 VusList 序列化为合法 JSON 串
 *   3) vus_plugin_run_vux_inproc：进程内调用 .vux 插件，返回非空字符串
 *
 * 编译（项目根目录，启用进程内嵌入）：
 *   gcc -DVUS_USE_PY $(python3-config --includes) -Irt \
 *       tests/test_plugin_inproc.c rt/libvus_rt.c \
 *       $(python3-config --ldflags) -o tests/test_plugin_inproc
 *
 * 运行（需设置插件目录与 libpython 搜索路径）：
 *   LD_LIBRARY_PATH=$(python3-config --prefix)/lib \
 *   VUS_PLUGIN_DIR=/workspace/vus/examples/plugins \
 *   ./tests/test_plugin_inproc
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../rt/libvus_rt.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

/* JSON 解析测试 */
static void test_json_parse(void) {
    printf("[test] vus_json_parse\n");
    VusString* json = vus_string_new("{\"a\":1,\"b\":[1,2,3]}");
    VusObject* obj = (VusObject*)vus_json_parse(json);
    CHECK(obj != NULL, "解析非空 JSON 返回非空容器");
    CHECK(obj && obj->type == TYPE_DICT, "顶层类型为字典");
    if (obj && obj->type == TYPE_DICT && obj->u.dict) {
        VusDict* dict = obj->u.dict;
        VusString* key_a = vus_string_new("a");
        VusString* key_b = vus_string_new("b");
        VusString* val_a = (VusString*)vus_dict_get(dict, key_a);
        CHECK(val_a && strcmp(vus_string_cstr(val_a), "1") == 0,
              "dict['a'] 取到字符串 '1'");
        void* val_b = vus_dict_get(dict, key_b);
        CHECK(val_b != NULL, "dict['b'] 非空");
        VusObject* bobj = (VusObject*)val_b;
        CHECK(bobj && bobj->type == TYPE_LIST, "dict['b'] 为列表容器");
        if (bobj && bobj->type == TYPE_LIST && bobj->u.list) {
            CHECK(vus_list_len(bobj->u.list) == 3, "列表长度为 3");
        }
        vus_unref(key_a);
        vus_unref(key_b);
    }
    vus_unref(json);
}

/* JSON 生成测试 */
static void test_json_generate(void) {
    printf("[test] vus_json_generate\n");
    VusObject* list_obj = (VusObject*)calloc(1, sizeof(VusObject));
    list_obj->magic = VUS_OBJECT_MAGIC;
    list_obj->type = TYPE_LIST;
    list_obj->u.list = vus_list_new(TYPE_MIXED);
    /* 依 vus_py_vus_to_py 约定，列表元素为 VusObject* 容器 */
    VusObject* e1 = (VusObject*)calloc(1, sizeof(VusObject));
    e1->magic = VUS_OBJECT_MAGIC;
    e1->type = TYPE_STR;
    e1->u.str = vus_string_new("hello");
    vus_list_append(list_obj->u.list, e1);
    VusObject* e2 = (VusObject*)calloc(1, sizeof(VusObject));
    e2->magic = VUS_OBJECT_MAGIC;
    e2->type = TYPE_STR;
    e2->u.str = vus_string_new("world");
    vus_list_append(list_obj->u.list, e2);

    VusString* out = vus_json_generate(list_obj);
    CHECK(out != NULL && strlen(vus_string_cstr(out)) > 0,
          "序列化列表返回非空 JSON 串");
    if (out && strlen(vus_string_cstr(out)) > 0) {
        const char* s = vus_string_cstr(out);
        CHECK(strstr(s, "hello") != NULL && strstr(s, "world") != NULL,
              "JSON 串包含元素内容");
        CHECK(s[0] == '[', "JSON 串以 '[' 开头");
    }
    vus_unref(out);
    vus_unref(list_obj->u.list);
    free(list_obj);
}

/* 进程内插件调用测试 */
static void test_plugin_inproc(void) {
    printf("[test] vus_plugin_run_vux_inproc\n");
    VusString* plugin = vus_string_new("示例");
    VusString* cmd = vus_string_new("echo");
    VusString* out = vus_plugin_run_vux_inproc(plugin, cmd);
    CHECK(out != NULL && strlen(vus_string_cstr(out)) > 0,
          "进程内调用示例插件返回非空字符串");
    if (out && strlen(vus_string_cstr(out)) > 0) {
        printf("  [INFO] 插件输出: %s\n", vus_string_cstr(out));
    }
    vus_unref(plugin);
    vus_unref(cmd);
    vus_unref(out);
}

int main(void) {
    printf("=== VUS 进程内插件/JSON 单元测试 ===\n");
    test_json_parse();
    test_json_generate();
    test_plugin_inproc();
    printf("=====================================\n");
    if (failures > 0) {
        printf("结果: %d 个断言失败\n", failures);
        return 1;
    }
    printf("结果: 全部通过\n");
    return 0;
}