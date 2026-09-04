/*
 * driver_vua_global.c — VUA 事件函数内全局变量验证驱动（桌面端）
 * 编译（分两步：先单独把生成的 C 的 main 改名为 vus_prog_main，避免 -Dmain= 波及驱动器自身）：
 *   gcc -I rt -Dmain=vus_prog_main -c 构建/test_vua_event_global.c -o /tmp/vus_evt.o
 *   gcc -I rt rt/vua.c tests/driver_vua_global.c /tmp/vus_evt.o \
 *       build/libvus_rt.a -o /tmp/vua_global_test -lm -ldl -lpthread
 * 运行：/tmp/vua_global_test
 */
#include <stdio.h>
#include <string.h>

#include "libvus_rt.h"
#include "vua.h"

/* 生成代码暴露的符号（生成器把 定义 main() 编成 int main(void)，经 -Dmain= 改为 vus_prog_main） */
int vus_prog_main(void);
extern VusString *vus__5168_5C40_8BA1_6570; /* 全局计数 */

int main(void) {
    VusString *_args[1] = {NULL};

    /* 1) 执行用户主流程：界面_显示 + 界面_绑定 两个事件 */
    vus_prog_main();

    printf("初始全局计数: %s\n", vus_string_cstr(vus__5168_5C40_8BA1_6570));

    VuaScreen *scr = vua_session_current(vua_global_session(NULL));
    if (!scr) { printf("FAIL: 无当前屏\n"); return 1; }
    VusDict *d = vus_dict_new();

    /* 2) 触发 事件_加 两次：期望 0 -> 2 */
    vua_trigger_event(scr, "加", d);
    vua_trigger_event(scr, "加", d);
    printf("两次 事件_加 后: %s（期望 2）\n", vus_string_cstr(vus__5168_5C40_8BA1_6570));

    /* 3) 触发 事件_乘2加1：期望 2 -> 5 */
    vua_trigger_event(scr, "乘2加1", d);
    printf("事件_乘2加1 后: %s（期望 5）\n", vus_string_cstr(vus__5168_5C40_8BA1_6570));

    /* 4) 触发带形参事件 事件_设值(值)：collect 键匹配 → 5 -> 42，无告警 */
    VusString *vk = vus_string_new("值");
    VusString *vv = vus_string_new("42");
    vus_dict_set(d, vk, vv);
    vua_trigger_event(scr, "设值", d);
    int ok = strcmp(vus_string_cstr(vus__5168_5C40_8BA1_6570), "42") == 0;
    printf("事件_设值(值=42) 后: %s（期望 42）%s\n",
           vus_string_cstr(vus__5168_5C40_8BA1_6570), ok ? "✅" : "❌");

    /* 5) 触发形参未匹配：collect 键为 'x'（形参是 '值'）→ 应出现告警且参数为 NULL */
    VusString *xk = vus_string_new("x");
    VusString *xv = vus_string_new("9");
    VusDict *d2 = vus_dict_new();
    vus_dict_set(d2, xk, xv);
    vua_trigger_event(scr, "设值", d2);
    printf("事件_设值(键x不匹配) 后: %s（值应变 0：参数未匹配→NULL，上方有告警）\n",
           vus_string_cstr(vus__5168_5C40_8BA1_6570));

    printf(ok ? "✅ 事件函数内全局变量读写正常（含日志/告警输出见上）\n" : "❌ 事件函数内全局变量异常\n");
    return ok ? 0 : 1;
}