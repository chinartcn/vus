/*
 * VUS XYZ 体感音游 —— 运行时内建实现
 *
 * 提供体感音游所需的四个内建能力（统一返回 VusString*，可作表达式或语句）：
 *   vus_clock_ms()           时钟()——单调毫秒基准，供判定时间差
 *   vus_sensor_read(axis)    传感器_读("x"/"y"/"z")——加速度计三轴，返回 milli-g 整数
 *   vus_audio_open(path)     音频_打开(path)——启动后台播放器并建立 IPC 控制
 *   vus_audio_play()         音频_播放()
 *   vus_audio_pause()        音频_暂停()
 *   vus_audio_resume()       音频_续()
 *   vus_audio_seek(ms)       音频_跳转(ms)
 *   vus_audio_position()     音频_进度()——毫秒
 *   vus_audio_duration()     音频_时长()——毫秒
 *
 * 数值约定：传感器与 target 统一用 milli-g 整数（1g = 1000），
 * 以免 VUS 判定表达式里引入浮点。（termux-sensor 给出的加速度为 m/s²，
 * 内部除以 9.81 得 g，再乘 1000。）
 *
 * 传感器后端：常驻后台 termux-sensor 进程，管道流式缓存最新 XYZ。
 * 音频后端  ：spawn mpv（--input-ipc-server）通过 UNIX 域 socket 发 JSON 命令，
 *             以支持播放/暂停/seek/进度 的精确控制。mpv 不可用时音频操作安全返回 0。
 */
#define _GNU_SOURCE  /* -std=c11 下启用 strtok_r/nanosleep/kill 等 POSIX/GNU 接口 */
#include "libvus_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

/* ==================== 工具：毫秒时钟 ==================== */
VusString* vus_clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return vus_to_string(ms);
}

/* ==================== 传感器：termux-sensor 常驻进程 ====================
 * 首次调用时 fork 后台 termux-sensor 流式输出，之后每次调用从管道读
 * 最近若干行，解析加速计 values 缓存三轴。传感器不可用时返回 0（milli-g）。 */
static pid_t   g_sens_pid = 0;
static int     g_sens_fd  = -1;
static double  g_sens[3] = {0.0, 0.0, 0.0};   /* x, y, z (m/s²，未换算) */

static void sensor_spawn(void)
{
    int pfd[2];
    if (pipe(pfd) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return; }
    if (pid == 0) {
        /* 子进程：termux-sensor 持续输出加速度计，写管道 */
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]); close(pfd[1]);
        execlp("termux-sensor", "termux-sensor",
               "-s", "accelerometer", "-n", "100000", "-d", "100", (char*)NULL);
        _exit(127);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL, 0) | O_NONBLOCK);
    g_sens_pid = pid;
    g_sens_fd  = pfd[0];
}

/* 从一行 termux-sensor JSON 提取 values 前三个元素（加速计 x,y,z）。*/
static void sensor_parse_line(const char* line)
{
    const char* p = strstr(line, "\"values\"");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;
    double vals[3] = {0, 0, 0};
    int n = 0;
    while (*p && *p != ']' && n < 3) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (*p == ']' || *p == '\0') break;
        double v = strtod(p, (char**)&p);
        vals[n++] = v;
    }
    if (n >= 3) { g_sens[0] = vals[0]; g_sens[1] = vals[1]; g_sens[2] = vals[2]; }
}

VusString* vus_sensor_read(const char* axis)
{
    if (g_sens_pid == 0) sensor_spawn();
    if (g_sens_fd >= 0) {
        char buf[4096];
        ssize_t r;
        while ((r = read(g_sens_fd, buf, sizeof(buf) - 1)) > 0) {
            buf[r] = '\0';
            char* save = NULL;
            for (char* tok = strtok_r(buf, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save)) {
                sensor_parse_line(tok);
            }
        }
    }
    int idx = 0;
    if (axis && axis[0] == 'y') idx = 1;
    else if (axis && axis[0] == 'z') idx = 2;
    /* m/s² → milli-g：除以 9.81 得 g，乘 1000 */
    int64_t millig = (int64_t)((g_sens[idx] / 9.81) * 1000.0);
    return vus_to_string(millig);
}

/* ==================== 音频：mpv IPC 控制 ==================== */
#define MPV_SOCK "/tmp/vus_xyz_audio.sock"
static int  g_mpv_fd  = -1;
static pid_t g_mpv_pid = 0;

/* 连接 mpv 的 IPC socket。返回 0 表示失败。*/
static int mpv_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, MPV_SOCK, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    return fd;
}

/* 发送一条命令并丢弃回复。返回 0 表示成功。*/
static int mpv_send(const char* cmd)
{
    if (g_mpv_fd < 0) return -1;
    size_t n = strlen(cmd);
    if (write(g_mpv_fd, cmd, n) != (ssize_t)n) return -1;
    if (write(g_mpv_fd, "\n", 1) != 1) return -1;
    char buf[512];
    read(g_mpv_fd, buf, sizeof(buf)); /* 排空一条回复 */
    return 0;
}

/* 发送 get 命令，返回回复中第一个数值；失败返回 0。*/
static int64_t mpv_get_num(const char* property)
{
    if (g_mpv_fd < 0) return 0;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "{\"command\":[\"get_property\",\"%s\"]}", property);
    size_t n = strlen(cmd);
    if (write(g_mpv_fd, cmd, n) != (ssize_t)n) return 0;
    if (write(g_mpv_fd, "\n", 1) != 1) return 0;
    char buf[1024];
    ssize_t r = read(g_mpv_fd, buf, sizeof(buf) - 1);
    if (r <= 0) return 0;
    buf[r] = '\0';
    /* 回复形如 {"data":<number>,"error":"success"}，截取第一个非 '{' 的数字。*/
    char* p = strchr(buf, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '"') p++;
    if (*p < '0' || *p > '9') return 0;
    double v = strtod(p, NULL);
    return (int64_t)v;
}

/* 等待 mpv 的 IPC socket 出现（最多约 1 秒）。*/
static int mpv_wait_ready(void)
{
    for (int i = 0; i < 50; i++) {
        if (access(MPV_SOCK, F_OK) == 0) return 0;
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

VusString* vus_audio_open(const char* path)
{
    if (g_mpv_pid > 0) {
        kill(g_mpv_pid, SIGKILL);
        waitpid(g_mpv_pid, NULL, 0);
    }
    if (g_mpv_fd >= 0) { close(g_mpv_fd); g_mpv_fd = -1; }
    unlink(MPV_SOCK);

    pid_t pid = fork();
    if (pid < 0) return vus_to_string(0);
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
        char ipc[128];
        snprintf(ipc, sizeof(ipc), "--input-ipc-server=%s", MPV_SOCK);
        execlp("mpv", "mpv", "--no-video", "--quiet", ipc, path, (char*)NULL);
        _exit(127);
    }
    g_mpv_pid = pid;
    if (mpv_wait_ready() != 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); g_mpv_pid = 0; return vus_to_string(0); }
    g_mpv_fd = mpv_connect();
    if (g_mpv_fd < 0) return vus_to_string(0);
    return vus_to_string(1);
}

VusString* vus_audio_play(void)     { return vus_to_string(mpv_send("{\"command\":[\"set_property\",\"pause\",false]}") == 0 ? 1 : 0); }
VusString* vus_audio_pause(void)    { return vus_to_string(mpv_send("{\"command\":[\"set_property\",\"pause\",true ]}") == 0 ? 1 : 0); }
VusString* vus_audio_resume(void)   { return vus_to_string(mpv_send("{\"command\":[\"set_property\",\"pause\",false]}") == 0 ? 1 : 0); }

VusString* vus_audio_seek(int64_t ms)
{
    /* mpv seek 用秒（浮点），"absolute+exact" 精确跳转。 */
    char cmd[128];
    double sec = (double)ms / 1000.0;
    snprintf(cmd, sizeof(cmd), "{\"command\":[\"seek\",%.3f,\"absolute+exact\"]}", sec);
    return vus_to_string(mpv_send(cmd) == 0 ? 1 : 0);
}

VusString* vus_audio_position(void) { return vus_to_string(mpv_get_num("time-pos") * 1000); }
VusString* vus_audio_duration(void) { return vus_to_string(mpv_get_num("duration") * 1000); }