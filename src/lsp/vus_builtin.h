/*
 * vus_builtin.h — VUS 内置函数权威元数据表（供 LSP 补全使用）
 *
 * 每个内置函数都包含：名称、完整签名字符串、中文说明、示例代码、类别。
 * 该表是 LSP 普通补全 / `.:` 详细补全的数据来源。
 */

#ifndef VUS_LSP_VUS_BUILTIN_H
#define VUS_LSP_VUS_BUILTIN_H

/* 内置函数类别（用于补全分组，category 字段） */
enum {
    VUS_BUILTIN_GRAPHIC = 1,   /* 图形_* 家族 */
    VUS_BUILTIN_IO,            /* 输入输出 / 运算工具 */
    VUS_BUILTIN_JSON,          /* JSON_* / 字典_键 / typeof */
    VUS_BUILTIN_DATE,          /* 日期_* 家族 */
    VUS_BUILTIN_LOG,           /* 日志_* 分级日志（EasyLogger） */
    VUS_BUILTIN_AUDIO,         /* 音频_* / 传感器_读 / 时钟 */
    VUS_BUILTIN_TUI,           /* tui_* 终端 UI */
    VUS_BUILTIN_NET,           /* 网络_* HTTP 请求 */
    VUS_BUILTIN_FILE,          /* 文件_* 文件操作 */
    VUS_BUILTIN_SHELL,         /* 命令_执行 / 文本_分割 */
    VUS_BUILTIN_PLUGIN,        /* 插件_* 插件调用 */
    VUS_BUILTIN_TERMUX,        /* Termux_* 环境启动 */
    VUS_BUILTIN_VUA            /* 界面_* VUA Android 组件流 */
};

/* 单个内置函数元数据 */
typedef struct {
    const char *name;       /* VUS 函数全名，如 "图形_矩形" */
    const char *signature;  /* 完整签名字符串，如 "图形_矩形(x, y, 宽, 高, 颜色)" */
    const char *doc;        /* 中文功能说明 */
    const char *example;    /* 中文示例代码片段 */
    int category;           /* 类别（见 VUS_BUILTIN_*） */
} VusBuiltin;

/* 返回内置函数元数据表（静态数组，可直接遍历） */
const VusBuiltin *vus_builtin_table(void);

/* 返回内置函数总数 */
int vus_builtin_count(void);

/* 按名称精确查找内置函数；找不到返回 NULL */
const VusBuiltin *vus_builtin_find(const char *name);

#endif /* VUS_LSP_VUS_BUILTIN_H */