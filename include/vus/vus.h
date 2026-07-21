/*
 * vus.h — VUS 编译器公共接口
 *
 * 编译器顶层入口，封装完整的编译流水线。
 */

#ifndef VUS_VUS_H
#define VUS_VUS_H

#include "../src/config.h"
#include "../src/token.h"
#include "../src/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 编译结果结构体 */
typedef struct {
    int   success;
    char  error_msg[512];
    char  c_output_path[1024];   /* 生成的 C 文件路径 */
    char  exe_output_path[1024]; /* 可执行文件路径 */
} VusResult;

/* 编译单个 .vus 文件，生成 C 代码 */
VusResult vus_compile_to_c(const char *vus_file_path, VusConfig *config);

/* 编译 .vus 文件并生成可执行文件 */
VusResult vus_compile_to_exe(const char *vus_file_path, VusConfig *config);

/* 编译并运行 */
int vus_run(const char *vus_file_path, VusConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* VUS_VUS_H */