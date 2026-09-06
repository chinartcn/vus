/* config.h — VUS vendored Oniguruma 6.9.9 最小配置
 * 由 config.h.cmake.in 的常见 Linux/GCC 取值手工生成（不做 autotools/cmake）。
 * 仅启用 UTF-8/Unicode 编码与核心正则能力；编译开关见各源文件。 */
#ifndef VUS_ONIGURUMA_CONFIG_H
#define VUS_ONIGURUMA_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1

#define PACKAGE "oniguruma"
#define PACKAGE_VERSION "6.9.9"
#define VERSION "6.9.9"

#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOIDP 8

/* 保留默认回溯/组合爆炸防护（不定义 USE_COMBINATION_EXPLOSION_CHECK 等） */

#endif