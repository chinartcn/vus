/*
 * main.c — VUS 编译器 CLI 入口
 *
 * 命令行接口入口点，处理编译、运行、测试、初始化等子命令。
 * 包括 vus_compile_to_c、vus_compile_to_exe、vus_run 等公共 API 的实现。
 */

#define _GNU_SOURCE
#include "generator.h"
#include "parser.h"
#include "lexer.h"
#include "config.h"
#include "../include/vus/vus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>

/* ============ 文件读取辅助函数 ============ */

static char *read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);

    char *data = (char *)malloc((size_t)len + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }

    size_t nread = fread(data, 1, (size_t)len, fp);
    fclose(fp);
    data[nread] = '\0';

    if (out_len) *out_len = nread;
    return data;
}

/* ============ 获取项目目录 ============ */

/* 编译器安装目录 */
static char g_compiler_dir[1024] = "";

/* 从 argv[0] 解析编译器安装目录 */
static int find_compiler_dir(const char *argv0) {
    char *resolved = realpath(argv0, NULL);
    if (!resolved) return 0;
    char *last_slash = strrchr(resolved, '/');
    if (!last_slash) {
        free(resolved);
        return 0;
    }
    *last_slash = '\0';
    strncpy(g_compiler_dir, resolved, sizeof(g_compiler_dir) - 1);
    g_compiler_dir[sizeof(g_compiler_dir) - 1] = '\0';
    free(resolved);
    return 1;
}

/* 将 config->rt_dir 设为编译器自带的运行时库绝对路径 */
static void config_set_compiler_rt(VusConfig *config) {
    if (g_compiler_dir[0]) {
        snprintf(config->rt_dir, sizeof(config->rt_dir), "%s/rt", g_compiler_dir);
    }
}

/* 从文件路径中提取项目目录（查找 vus.json 所在目录） */
static int find_project_dir(const char *vus_file, char *project_dir, size_t dir_size) {
    /* 先从当前目录查找 vus.json */
    struct stat st;
    if (stat("vus.json", &st) == 0) {
        /* 获取当前目录的绝对路径 */
        if (getcwd(project_dir, dir_size)) {
            return 1;
        }
    }

    /* 从文件路径中推断 */
    if (vus_file) {
        char *copy = strdup(vus_file);
        if (!copy) return 0;
        char *last_slash = strrchr(copy, '/');
        if (last_slash) {
            *last_slash = '\0';
            /* 检查该目录下是否有 vus.json */
            char test_path[2048];
            snprintf(test_path, sizeof(test_path), "%s/vus.json", copy);
            if (stat(test_path, &st) == 0) {
                strncpy(project_dir, copy, dir_size - 1);
                project_dir[dir_size - 1] = '\0';
                free(copy);
                return 1;
            }
            /* 继续向上查找 */
            free(copy);
            return find_project_dir(NULL, project_dir, dir_size);
        }
        free(copy);
    }

    /* 默认使用当前目录 */
    if (getcwd(project_dir, dir_size)) {
        return 1;
    }
    return 0;
}

/* ============ vus_compile_to_c ============ */

VusResult vus_compile_to_c(const char *vus_file_path, VusConfig *config) {
    VusResult result;
    memset(&result, 0, sizeof(result));

    if (!vus_file_path || !config) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Invalid arguments to vus_compile_to_c");
        return result;
    }

    /* 读取源码 */
    size_t source_len = 0;
    char *source = read_file(vus_file_path, &source_len);
    if (!source) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Failed to read source file: %s", vus_file_path);
        return result;
    }

    /* 词法分析 */
    VusLexer *lexer = vus_lexer_new(source, source_len, config->style);
    if (!lexer) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Failed to create lexer");
        free(source);
        return result;
    }

    size_t token_count = 0;
    VusToken *tokens = vus_lexer_tokenize(lexer, &token_count);

    if (lexer->error) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Lexer error: %s", vus_lexer_error(lexer));
        vus_lexer_free_tokens(tokens, token_count);
        vus_lexer_free(lexer);
        free(source);
        return result;
    }
    /* 从词法分析器接管 Token 所有权 */
    tokens = vus_lexer_steal_tokens(lexer, &token_count);
    vus_lexer_free(lexer);

    /* 语法分析 */
    VusParser *parser = vus_parser_new(tokens, token_count, config->style);
    if (!parser) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Failed to create parser");
        vus_lexer_free_tokens(tokens, token_count);
        free(source);
        return result;
    }

    VusAstProgram *program = vus_parser_parse(parser);

    if (parser->error) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Parser error: %s", vus_parser_error(parser));
        vus_parser_free(parser);
        vus_lexer_free_tokens(tokens, token_count);
        free(source);
        return result;
    }
    vus_parser_free(parser);
    vus_lexer_free_tokens(tokens, token_count);

    /* 生成 C 代码 */
    char *c_code = vus_generate_c(program, config);
    if (!c_code) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Failed to generate C code");
        vus_ast_node_free((VusAstNode *)program);
        free(source);
        return result;
    }

    /* 确定输出路径 */
    char build_dir[1024];
    config->build_dir[0] ? snprintf(build_dir, sizeof(build_dir), "%s", config->build_dir)
                         : snprintf(build_dir, sizeof(build_dir), "%s/构建", config->project_dir);

    /* 确保构建目录存在 */
    struct stat st;
    if (stat(build_dir, &st) != 0) {
        mkdir(build_dir, 0755);
    }

    /* 提取文件名（不含扩展名） */
    const char *basename = strrchr(vus_file_path, '/');
    basename = basename ? basename + 1 : vus_file_path;
    char name_buf[512];
    strncpy(name_buf, basename, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    char *dot = strrchr(name_buf, '.');
    if (dot) *dot = '\0';

    /* 写入 C 文件 */
    char c_output_path[1024];
    snprintf(c_output_path, sizeof(c_output_path), "%s/%s.c", build_dir, name_buf);

    FILE *fp = fopen(c_output_path, "w");
    if (!fp) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Failed to write C file: %s", c_output_path);
        vus_generate_free(c_code);
        vus_ast_node_free((VusAstNode *)program);
        free(source);
        return result;
    }

    size_t c_len = strlen(c_code);
    fwrite(c_code, 1, c_len, fp);
    fclose(fp);

    /* 填充结果 */
    result.success = 1;
    strncpy(result.c_output_path, c_output_path, sizeof(result.c_output_path) - 1);
    result.c_output_path[sizeof(result.c_output_path) - 1] = '\0';

    /* 清理 */
    vus_generate_free(c_code);
    vus_ast_node_free((VusAstNode *)program);
    free(source);

    return result;
}

/* ============ vus_compile_to_exe ============ */

VusResult vus_compile_to_exe(const char *vus_file_path, VusConfig *config) {
    VusResult result;

    /* 先编译到 C */
    result = vus_compile_to_c(vus_file_path, config);
    if (!result.success) {
        return result;
    }

    /* 确定可执行文件输出路径 */
    const char *basename = strrchr(vus_file_path, '/');
    basename = basename ? basename + 1 : vus_file_path;
    char name_buf[512];
    strncpy(name_buf, basename, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    char *dot = strrchr(name_buf, '.');
    if (dot) *dot = '\0';

    char exe_output_path[1024];
    char build_dir[1024];
    config->build_dir[0] ? snprintf(build_dir, sizeof(build_dir), "%s", config->build_dir)
                         : snprintf(build_dir, sizeof(build_dir), "%s/构建", config->project_dir);
    snprintf(exe_output_path, sizeof(exe_output_path), "%s/%s", build_dir, name_buf);

    /* 调用 GCC 编译 */
    char error_msg[512];
    int compile_result = vus_compile_c(result.c_output_path, exe_output_path,
                                        config, error_msg, sizeof(error_msg));

    if (compile_result != 0) {
        result.success = 0;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "GCC compilation failed: %s", error_msg);
        return result;
    }

    strncpy(result.exe_output_path, exe_output_path, sizeof(result.exe_output_path) - 1);
    result.exe_output_path[sizeof(result.exe_output_path) - 1] = '\0';
    result.success = 1;

    return result;
}

/* ============ vus_run ============ */

int vus_run(const char *vus_file_path, VusConfig *config) {
    VusResult result = vus_compile_to_exe(vus_file_path, config);

    if (!result.success) {
        fprintf(stderr, "Compilation failed: %s\n", result.error_msg);
        return 1;
    }

    /* 运行可执行文件 */
    int exit_code = system(result.exe_output_path);

    /* 返回退出码（WEXITSTATUS 提取实际退出码） */
    if (exit_code >= 0 && WIFEXITED(exit_code)) {
        return WEXITSTATUS(exit_code);
    }
    return exit_code;
}

/* ============ vus init 实现 ============ */

static int vus_init(int force) {
    /* 检查是否已存在 vus.json */
    struct stat st;
    if (!force && stat("vus.json", &st) == 0) {
        printf("vus.json 已存在。使用 --force 重新初始化。\n");
        return 0;
    }

    printf("==== VUS 项目初始化向导 v0.1 ====\n");
    printf("本向导将为你的项目配置基础代码风格，选定后全局锁定。\n\n");

    printf("【适配推荐】\n");
    printf("  1. 函数风格：熟悉 Python、有编程基础，可中英混写\n");
    printf("  2. 易语言风格：零基础，完全不想使用任何英文符号\n\n");

    printf("【风格示例展示】\n");
    printf("  [1] 函数风格\n");
    printf("      定义 求和(a, b):\n");
    printf("          返回 a + b\n");
    printf("      打印(求和(1, 2))\n\n");
    printf("  [2] 易语言风格\n");
    printf("      .功能 求和(a, b)\n");
    printf("          .返回 a + b\n");
    printf("      .结束\n");
    printf("      .打印(.求和(1, 2))\n\n");

    /* 选择风格 */
    int style_choice = 0;
    char input[64];
    printf("请输入选择编号 [1/2]：\n> ");
    if (!fgets(input, sizeof(input), stdin)) {
        fprintf(stderr, "输入错误\n");
        return 1;
    }
    style_choice = atoi(input);

    const char *style_name = NULL;
    if (style_choice == 1) {
        style_name = "函数";
    } else if (style_choice == 2) {
        style_name = "易语言";
    } else {
        fprintf(stderr, "无效选择，默认使用「函数风格」\n");
        style_name = "函数";
    }

    /* 确认 */
    printf("\n已选择「%s风格」，确认保存该配置？(y/n)\n> ", style_name);
    if (!fgets(input, sizeof(input), stdin)) {
        fprintf(stderr, "输入错误\n");
        return 1;
    }
    if (input[0] != 'y' && input[0] != 'Y') {
        printf("已取消初始化。\n");
        return 0;
    }

    /* 生成 vus.json */
    const char *json_template =
        "{\n"
        "    \"name\": \"我的项目\",\n"
        "    \"version\": \"0.1.0\",\n"
        "    \"风格\": \"%s\",\n"
        "    \"主文件\": \"main.vus\",\n"
        "    \"输出模式\": \"c\",\n"
        "    \"列表模式\": \"严格\",\n"
        "    \"调试\": false,\n"
        "    \"目标平台\": \"linux-gnu\",\n"
        "    \"依赖\": [],\n"
        "    \"编译选项\": {\n"
        "        \"优化\": \"速度\",\n"
        "        \"ARM版本\": \"ARM64\"\n"
        "    }\n"
        "}\n";

    char json_content[2048];
    snprintf(json_content, sizeof(json_content), json_template, style_name);

    FILE *fp = fopen("vus.json", "w");
    if (!fp) {
        fprintf(stderr, "无法写入 vus.json\n");
        return 1;
    }
    fputs(json_content, fp);
    fclose(fp);

    printf("\n配置写入 vus.json 完成。\n");

    /* 创建基本项目结构 */
    struct {
        const char *path;
        int is_dir;
        const char *content;
    } items[] = {
        {"main.vus", 0, "#// 主程序入口\n\n"},
        {"测试", 1, NULL},
        {"构建", 1, NULL},
        {"libs", 1, NULL},
        {NULL, 0, NULL}
    };

    for (int i = 0; items[i].path; i++) {
        if (items[i].is_dir) {
            if (stat(items[i].path, &st) != 0) {
                mkdir(items[i].path, 0755);
                printf("创建目录 %s/\n", items[i].path);
            }
        } else {
            if (stat(items[i].path, &st) != 0) {
                fp = fopen(items[i].path, "w");
                if (fp) {
                    if (items[i].content) fputs(items[i].content, fp);
                    fclose(fp);
                    printf("创建文件 %s\n", items[i].path);
                }
            }
        }
    }

    printf("\n项目初始化完成！\n");
    printf("运行 `vus build --c-only main.vus` 编译为 C 代码。\n");
    printf("运行 `vus build --exe main.vus` 编译为可执行文件。\n");
    printf("运行 `vus run main.vus` 编译并运行。\n");

    return 0;
}

/* ============ vus test 实现 ============ */

static int vus_test(void) {
    const char *test_dir = "测试";
    struct stat st;

    if (stat(test_dir, &st) != 0) {
        fprintf(stderr, "测试目录「测试/」不存在。\n");
        return 1;
    }

    /* 扫描测试目录 */
    DIR *dir = opendir(test_dir);
    if (!dir) {
        fprintf(stderr, "无法打开测试目录「测试/」\n");
        return 1;
    }

    /* 收集测试文件 */
    char **test_files = NULL;
    size_t test_count = 0;
    size_t test_cap = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG && entry->d_type != DT_LNK) continue;
        const char *name = entry->d_name;
        size_t nlen = strlen(name);
        if (nlen > 8 && memcmp(name, "test_", 5) == 0 &&
            memcmp(name + nlen - 4, ".vus", 4) == 0) {
            if (test_count >= test_cap) {
                test_cap = test_cap ? test_cap * 2 : 16;
                test_files = (char **)realloc(test_files, test_cap * sizeof(char *));
            }
            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s/%s", test_dir, name);
            test_files[test_count] = strdup(full_path);
            test_count++;
        }
    }
    closedir(dir);

    if (test_count == 0) {
        printf("没有找到测试用例（test_*.vus）\n");
        free(test_files);
        return 0;
    }

    /* 加载配置 */
    VusConfig config;
    memset(&config, 0, sizeof(config));
    if (vus_config_load(&config, ".") != 0) {
        /* 使用默认配置 */
        config.project_dir[0] = '.';
        config.project_dir[1] = '\0';
        strcpy(config.style, "函数");
        strcpy(config.rt_dir, "rt");
        strcpy(config.build_dir, "构建");
        strcpy(config.optimization, "速度");
    }
    config_set_compiler_rt(&config);

    /* 排序测试文件（按文件名升序） */
    for (size_t i = 0; i < test_count; i++) {
        for (size_t j = i + 1; j < test_count; j++) {
            if (strcmp(test_files[i], test_files[j]) > 0) {
                char *tmp = test_files[i];
                test_files[i] = test_files[j];
                test_files[j] = tmp;
            }
        }
    }

    /* 运行测试 */
    int passed = 0;
    int failed = 0;
    char failed_names[1024] = {0};

    for (size_t i = 0; i < test_count; i++) {
        const char *fname = test_files[i];
        const char *short_name = strrchr(fname, '/');
        short_name = short_name ? short_name + 1 : fname;

        printf("测试 [%zu/%zu]: %s ... ", i + 1, test_count, short_name);
        fflush(stdout);

        /* 编译并运行 */
        int exit_code = vus_run(fname, &config);

        if (exit_code == 0) {
            printf("通过\n");
            passed++;
        } else {
            printf("失败 (退出码: %d)\n", exit_code);
            failed++;
            if (failed_names[0]) {
                size_t flen = strlen(failed_names);
                snprintf(failed_names + flen, sizeof(failed_names) - flen,
                         ", %s", short_name);
            } else {
                snprintf(failed_names, sizeof(failed_names), "%s", short_name);
            }
        }
    }

    /* 清理 */
    for (size_t i = 0; i < test_count; i++) {
        free(test_files[i]);
    }
    free(test_files);

    /* 输出汇总 */
    printf("\n测试完成：共 %zu 个用例，通过 %d 个，失败 %d 个\n",
           test_count, passed, failed);
    if (failed > 0) {
        printf("失败用例：%s\n", failed_names);
        return 1;
    }

    return 0;
}

/* ============ 帮助信息 ============ */

static void print_help(void) {
    printf("VUS 编译器 v0.1\n");
    printf("用法: vus <命令> [选项]\n\n");
    printf("命令:\n");
    printf("  build --c-only <file>   编译为 C 代码\n");
    printf("  build --exe   <file>   编译为可执行文件\n");
    printf("  run           <file>   编译并运行\n");
    printf("  init                   交互式项目初始化\n");
    printf("  test                   运行测试\n");
    printf("  --help, -h             显示此帮助\n\n");
    printf("示例:\n");
    printf("  vus init\n");
    printf("  vus build --c-only main.vus\n");
    printf("  vus build --exe main.vus\n");
    printf("  vus run main.vus\n");
    printf("  vus test\n");
}

/* ============ main 函数 ============ */

int main(int argc, char *argv[]) {
    /* 解析编译器安装目录 */
    find_compiler_dir(argv[0]);

    if (argc < 2) {
        print_help();
        return 0;
    }

    const char *cmd = argv[1];

    /* --help */
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_help();
        return 0;
    }

    /* init */
    if (strcmp(cmd, "init") == 0) {
        int force = 0;
        if (argc > 2 && strcmp(argv[2], "--force") == 0) {
            force = 1;
        }
        return vus_init(force);
    }

    /* test */
    if (strcmp(cmd, "test") == 0) {
        return vus_test();
    }

    /* build */
    if (strcmp(cmd, "build") == 0) {
        if (argc < 4) {
            fprintf(stderr, "用法: vus build --c-only|--exe <file>\n");
            return 1;
        }

        const char *mode = argv[2];
        const char *file = argv[3];

        /* 加载配置 */
        VusConfig config;
        memset(&config, 0, sizeof(config));

        char project_dir[1024];
        if (!find_project_dir(file, project_dir, sizeof(project_dir))) {
            fprintf(stderr, "无法确定项目目录\n");
            return 1;
        }

        if (vus_config_load(&config, project_dir) != 0) {
            /* 使用默认配置 */
            strncpy(config.project_dir, project_dir, sizeof(config.project_dir) - 1);
            config.project_dir[sizeof(config.project_dir) - 1] = '\0';
            strcpy(config.style, "函数");
            strcpy(config.rt_dir, "rt");
            strcpy(config.build_dir, "构建");
            strcpy(config.optimization, "速度");
        }
        config_set_compiler_rt(&config);

        if (strcmp(mode, "--c-only") == 0) {
            VusResult result = vus_compile_to_c(file, &config);
            if (result.success) {
                printf("C 代码已生成: %s\n", result.c_output_path);
                return 0;
            } else {
                fprintf(stderr, "编译失败: %s\n", result.error_msg);
                return 1;
            }
        } else if (strcmp(mode, "--exe") == 0) {
            VusResult result = vus_compile_to_exe(file, &config);
            if (result.success) {
                printf("可执行文件已生成: %s\n", result.exe_output_path);
                return 0;
            } else {
                fprintf(stderr, "编译失败: %s\n", result.error_msg);
                return 1;
            }
        } else {
            fprintf(stderr, "未知编译模式: %s\n", mode);
            fprintf(stderr, "用法: vus build --c-only|--exe <file>\n");
            return 1;
        }
    }

    /* run */
    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "用法: vus run <file>\n");
            return 1;
        }

        const char *file = argv[2];

        VusConfig config;
        memset(&config, 0, sizeof(config));

        char project_dir[1024];
        if (!find_project_dir(file, project_dir, sizeof(project_dir))) {
            fprintf(stderr, "无法确定项目目录\n");
            return 1;
        }

        if (vus_config_load(&config, project_dir) != 0) {
            strncpy(config.project_dir, project_dir, sizeof(config.project_dir) - 1);
            config.project_dir[sizeof(config.project_dir) - 1] = '\0';
            strcpy(config.style, "函数");
            strcpy(config.rt_dir, "rt");
            strcpy(config.build_dir, "构建");
            strcpy(config.optimization, "速度");
        }
        config_set_compiler_rt(&config);

        return vus_run(file, &config);
    }

    /* 未知命令 */
    fprintf(stderr, "未知命令: %s\n", cmd);
    fprintf(stderr, "使用 'vus --help' 查看帮助\n");
    return 1;
}