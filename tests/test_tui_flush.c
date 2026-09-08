/* test_tui_flush.c — TUI 画布子系统回归测试（pty 下验证差分刷新定位 + 读键边界）
 *
 * 覆盖两个已修复缺陷：
 *   1. 差分刷新缺少光标定位：行中局部变更（首帧之后）会被写到错误列，
 *      画面错位（vus_tui_flush 仅在 c==0 时定位）。
 *   2. vus_tui_read_key 满 16 字节输入时 buf[16]='\0' 栈越界写（缓冲 17 字节）。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pty.h>
#include "vus_tui.h"

static int g_fail = 0;

static void check(int cond, const char *msg) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); g_fail = 1; }
    else fprintf(stderr, "ok: %s\n", msg);
}

int main(void) {
    int real_out = dup(STDOUT_FILENO);
    FILE *out = fdopen(real_out, "w");
    int master = -1, slave = -1;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) { perror("openpty"); return 1; }
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDIN_FILENO);
    close(slave);

    /* ---- 1. 差分刷新定位 ----
     * 第 1 帧 (0,0) 写 "HELLO" 全量上屏；第 2 帧仅 (0,3) 改为 'X'。
     * 修复前：第 2 帧无 \033[1;4H 定位，'X' 落在第 1 列 → 画面错位。
     * 修复后：每个变更单元格前都有 \033[row;col H 定位。 */
    vus_tui_canvas_resize(3, 20);
    vus_tui_canvas_put(0, 0, "HELLO", -1, -1, 0);
    vus_tui_flush();
    usleep(50000);
    char f1[8192];
    int n1 = (int)read(master, f1, sizeof(f1) - 1);
    if (n1 < 0) n1 = 0;
    f1[n1] = '\0';
    check(strstr(f1, "HELLO") != NULL, "第1帧全量输出 HELLO");

    vus_tui_canvas_put(0, 3, "X", -1, -1, 0);
    vus_tui_flush();
    usleep(50000);
    char f2[8192];
    int n2 = (int)read(master, f2, sizeof(f2) - 1);
    if (n2 < 0) n2 = 0;
    f2[n2] = '\0';
    check(strstr(f2, "\033[1;4H") != NULL, "差分帧对行中变更输出 \033[1;4H 定位");
    check(strstr(f2, "X") != NULL, "差分帧输出变更内容 X");

    /* ---- 2. read_key 满 16 字节输入（修复前 buf[16]='\0' 栈越界写）---- */
    char payload[16];
    for (int i = 0; i < 16; i++) payload[i] = (char)('a' + i);
    if (write(master, payload, 16) != 16) { perror("write payload"); return 1; }
    VusString *s = vus_tui_read_key();
    check(s && strlen(vus_string_cstr(s)) == 16 && strncmp(vus_string_cstr(s), payload, 16) == 0,
          "read_key 一次读满 16 字节完整返回（无越界写）");

    fflush(out);
    close(master);
    fprintf(out, g_fail ? "test_tui_flush FAILED\n" : "test_tui_flush ok\n");
    return g_fail ? 1 : 0;
}
