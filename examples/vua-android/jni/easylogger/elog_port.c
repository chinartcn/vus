/*
 * VUS 平台适配层：为 EasyLogger 提供控制台(stdout) + 文件(追加) 双通道输出。
 *
 * 实现 EasyLogger 要求的 port 接口：
 *   elog_port_init / elog_port_deinit / elog_port_output
 *   elog_port_output_lock / elog_port_output_unlock
 *   elog_port_get_time / elog_port_get_p_info / elog_port_get_t_info
 *
 * 日志文件默认 "vus.log"（当前目录），可用环境变量 VUS_LOG_FILE 覆盖。
 * VUS 协程为单线程协作式调度，锁采用普通互斥即可满足跨线程安全。
 *
 * 许可：本文件为 VUS 项目新增，遵循 MIT；EasyLogger 本体许可见 easylogger/LICENSE。
 */

#define _GNU_SOURCE
#include <elog.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* ---- 文件输出通道 ---- */
static FILE *s_log_file = NULL;

/* ---- 输出锁（跨线程安全；VUS 为协作式单线程，无禁用期间递归问题） ---- */
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- 供 vus_log_* 查询日志文件是否就绪 ---- */
int vus_log_file_ready(void) {
    return s_log_file != NULL;
}

/* ---- EasyLogger port 接口 ---- */

ElogErrCode elog_port_init(void) {
    const char *path = getenv("VUS_LOG_FILE");
    if (!path || !path[0]) {
        path = "vus.log";
    }
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    s_log_file = fopen(path, "a");
    if (!s_log_file) {
        /* 打开日志文件失败不阻断控制台输出，降级为仅控制台 */
        return ELOG_NO_ERR;
    }
    setvbuf(s_log_file, NULL, _IOLBF, 0); /* 行缓冲，避免进程异常退出丢失 */
    return ELOG_NO_ERR;
}

ElogErrCode elog_port_deinit(void) {
    if (s_log_file) {
        fflush(s_log_file);
        fclose(s_log_file);
        s_log_file = NULL;
    }
    return ELOG_NO_ERR;
}

void elog_port_output(const char *log, size_t size) {
    if (!log || size == 0) return;
    /* 控制台 */
    fwrite(log, 1, size, stdout);
    fflush(stdout);
    /* 文件（追加） */
    if (s_log_file) {
        fwrite(log, 1, size, s_log_file);
        fflush(s_log_file);
    }
}

void elog_port_output_lock(void) {
    pthread_mutex_lock(&s_log_mutex);
}

void elog_port_output_unlock(void) {
    pthread_mutex_unlock(&s_log_mutex);
}

/* ---- 格式化信息（时间/进程/线程） ---- */

/* 返回静态缓冲的时间串，如 "2026-08-16 12:00:00.000" */
const char *elog_port_get_time(void) {
    static char buf[32];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_val;
    localtime_r(&ts.tv_sec, &tm_val);
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
             tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec,
             (int)(ts.tv_nsec / 1000000));
    return buf;
}

/* 进程信息：返回 PID 字符串 */
const char *elog_port_get_p_info(void) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)getpid());
    return buf;
}

/* 线程信息：返回线程 ID 字符串（VUS 协程无调度线程，返回主线程标记） */
const char *elog_port_get_t_info(void) {
    return "-";
}