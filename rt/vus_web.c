/*
 * vus_web.c — VUS 网页服务模块（极简 POSIX HTTP 服务器）
 *
 *   网页_服务(端口, 内容)     —— 内容模式：任意路径返回该 HTML（含三引号模板）
 *   网页_服务目录(目录, 端口) —— 目录模式：静态文件服务；目录下 网页.json 可配置
 *                                  {"入口":"index.html","端口":8080}
 *   网页_停止(端口)            —— 停止某端口上的服务（"1"/"0"）
 *
 * 设计：
 *  - 零外部依赖：只依赖 POSIX socket + pthread，与本仓库"桌面零依赖"传统一致；
 *     许可证干净（无 GPL 依赖），平台覆盖 Linux / macOS / Android Termux。
 *  - 每服务一个后台线程（独立数据，与协程无共享可变状态，线程安全）。
 *  - 目录模式做 URL 解码 + 路径规范化，拒绝 "../" 穿越，仅服务根目录内文件。
 *  - 成功返回可访问 URL 字符串；失败返回 {"错误": ...}（可 JSON 解析）。
 *  - 进程退出时监听线程随进程终止；网页_停止 可主动优雅关闭。
 */

#define _GNU_SOURCE  /* strdup / usleep 等 POSIX 扩展（-std=c11 下需要） */

#include "libvus_rt.h"
#include "yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define WEB_MAX   8
#define WEB_PORT_LEN 16
#define WEB_BUF   16384

typedef struct {
    int           in_use;
    int           port;
    int           fd;          /* 监听 socket；线程退出后置 -1 */
    volatile int  stop;        /* 停止标志 */
    pthread_t     thr;
    int           mode;        /* 0=内容 1=目录 */
    char         *content;     /* 内容模式：HTML 全文 */
    char         *root;        /* 目录模式：根目录绝对路径 */
    char         *entry;       /* 目录模式：入口相对路径（默认 index.html） */
} WebSrv;

static WebSrv           s_web[WEB_MAX];
static pthread_mutex_t  s_web_lock = PTHREAD_MUTEX_INITIALIZER;

static WebSrv *web_find_by_port(int port) {
    for (int i = 0; i < WEB_MAX; i++)
        if (s_web[i].in_use && s_web[i].port == port) return &s_web[i];
    return NULL;
}

static WebSrv *web_alloc(void) {
    for (int i = 0; i < WEB_MAX; i++) {
        if (!s_web[i].in_use) {
            memset(&s_web[i], 0, sizeof(s_web[i]));
            s_web[i].fd = -1;
            s_web[i].in_use = 1;
            return &s_web[i];
        }
    }
    return NULL;
}

static VusString *web_err(const char *msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"错误\":\"%s\"}", msg);
    return vus_string_new(buf);
}

/* ---- MIME 表 ---- */
static const char *web_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0 || strcmp(dot, ".mjs") == 0) return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".txt") == 0 || strcmp(dot, ".md") == 0) return "text/plain; charset=utf-8";
    if (strcmp(dot, ".xml") == 0 || strcmp(dot, ".pdf") == 0) return "application/octet-stream";
    if (strcmp(dot, ".png") == 0)  return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)  return "image/gif";
    if (strcmp(dot, ".svg") == 0)  return "image/svg+xml";
    if (strcmp(dot, ".webp") == 0) return "image/webp";
    if (strcmp(dot, ".ico") == 0)  return "image/x-icon";
    if (strcmp(dot, ".avif") == 0) return "image/avif";
    if (strcmp(dot, ".wasm") == 0) return "application/wasm";
    if (strcmp(dot, ".mp3") == 0)  return "audio/mpeg";
    if (strcmp(dot, ".mp4") == 0)  return "video/mp4";
    if (strcmp(dot, ".woff") == 0)  return "font/woff";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";
    if (strcmp(dot, ".ttf") == 0)   return "font/ttf";
    if (strcmp(dot, ".otf") == 0)   return "font/otf";
    return "application/octet-stream";
}

/* URL 百分号解码（原地，返回解码后长度） */
static int web_url_decode(char *s, int n) {
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = s[i+1], lo = s[i+2];
            int hv = (hi >= '0' && hi <= '9') ? hi - '0' :
                     (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 :
                     (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
            int lv = (lo >= '0' && lo <= '9') ? lo - '0' :
                     (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 :
                     (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
            if (hv >= 0 && lv >= 0) {
                s[o++] = (char)(hv * 16 + lv);
                i += 2;
                continue;
            }
        }
        s[o++] = s[i];
    }
    return o;
}

/* 拒绝目录穿越：路径中禁止出现 ".." 段 */
static int web_path_safe(const char *path) {
    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) return 0;
        p++;
    }
    return 1;
}

/* 发送原始字节（到缓冲耗尽或失败） */
static void web_send(int fd, const void *data, size_t n) {
    const char *p = (const char *)data;
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

static void web_send_cstr(int fd, const char *s) {
    web_send(fd, s, strlen(s));
}

static void web_status(int fd, int code, const char *reason) {
    char h[256];
    snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Connection: close\r\n\r\n", code, reason);
    web_send_cstr(fd, h);
}

/* 目录模式：发送文件；不存在返回 0，存在返回 1 */
static int web_send_file(int fd, WebSrv *w, const char *rel) {
    const char *entry = rel[0] ? rel : w->entry;
    if (web_path_safe(entry) == 0) { web_status(fd, 403, "Forbidden"); return 1; }
    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", w->root, entry);
    int f = open(full, O_RDONLY);
    if (f < 0) return 0;
    off_t sz = lseek(f, 0, SEEK_END);
    lseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
        "Connection: close\r\n\r\n",
        web_mime(entry), (long long)sz);
    web_send_cstr(fd, hdr);
    char buf[WEB_BUF];
    ssize_t n;
    while ((n = read(f, buf, sizeof(buf))) > 0) web_send(fd, buf, (size_t)n);
    close(f);
    return 1;
}

/* 处理单个连接 */
static void web_handle(WebSrv *w, int fd) {
    /* 设读超时，防止半连接挂死 */
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char line[8192];
    int len = 0;
    while (len < (int)sizeof(line) - 1) {
        ssize_t n = recv(fd, line + len, 1, 0);
        if (n <= 0) return;
        if (line[len] == '\n') break;
        len++;
    }
    line[len] = '\0';
    /* 行：METHOD SP URI SP HTTP/x.y */
    char *sp1 = strchr(line, ' ');
    if (!sp1) { web_status(fd, 400, "Bad Request"); return; }
    *sp1 = '\0';
    char *method = line;
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        web_status(fd, 405, "Method Not Allowed");
        return;
    }
    char *uri = sp1 + 1;
    char *sp2 = strchr(uri, ' ');
    if (sp2) *sp2 = '\0';
    /* 读完剩余请求头直到空行（不等待 body，避免触发读超时） */
    {
        int had_lf = 0;
        char ch;
        while (1) {
            ssize_t n = recv(fd, &ch, 1, 0);
            if (n <= 0) break;
            if (ch == '\n') { if (had_lf) break; had_lf = 1; }
            else if (ch != '\r') had_lf = 0;
        }
    }

    /* 只取路径（去掉 ?query） */
    char *q = strchr(uri, '?');
    if (q) *q = '\0';
    int ulen = (int)strlen(uri);
    ulen = web_url_decode(uri, ulen);
    uri[ulen] = '\0';
    if (ulen == 0) uri[0] = '/';

    if (w->mode == 0) {
        /* 内容模式：任意路径返回整段 HTML */
        size_t clen = strlen(w->content);
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n", clen);
        web_send_cstr(fd, hdr);
        if (strcmp(method, "HEAD") != 0) web_send(fd, w->content, clen);
        return;
    }

    /* 目录模式 */
    if (strcmp(uri, "/favicon.ico") == 0) { web_status(fd, 404, "Not Found"); return; }
    if (web_send_file(fd, w, uri + 1) == 0) {
        web_send_cstr(fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n404 Not Found");
    }
}

/* 后台服务线程 */
static void *web_loop(void *arg) {
    WebSrv *w = (WebSrv *)arg;
    while (!w->stop) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int cfd = accept(w->fd, (struct sockaddr *)&cli, &cl);
        if (cfd < 0) {
            if (w->stop) break;
            usleep(20000);
            continue;
        }
        web_handle(w, cfd);
        close(cfd);
    }
    return NULL;
}

/* 读取目录配置 网页.json（{"入口":...,"端口":...}），返回 malloc 配置或默认 */
static char *web_conf_value(const char *dir, const char *key, const char *dflt) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/网页.json", dir);
    FILE *f = fopen(path, "r");
    if (!f) return strdup(dflt);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return strdup(dflt); }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return strdup(dflt); }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    yyjson_doc *doc = yyjson_read(buf, rd, 0);
    free(buf);
    const char *val = NULL;
    char numbuf[32];
    if (doc) {
        yyjson_val *v = yyjson_obj_get(yyjson_doc_get_root(doc), key);
        if (yyjson_is_str(v)) val = yyjson_get_str(v);
        else if (yyjson_is_int(v)) { snprintf(numbuf, sizeof(numbuf), "%lld", (long long)yyjson_get_int(v)); val = numbuf; }
        else if (yyjson_is_uint(v)) { snprintf(numbuf, sizeof(numbuf), "%llu", (unsigned long long)yyjson_get_uint(v)); val = numbuf; }
        yyjson_doc_free(doc);
    }
    return strdup(val ? val : dflt);
}

/* 通用启动：mode=0 内容 / mode=1 目录 */
static VusString *web_start(int mode, const char *content, const char *dir,
                            const char *port_str) {
    int port = port_str ? atoi(port_str) : 0;

    if (mode == 0) {
        /* 内容模式：必须显式给端口 */
        if (port <= 0 || port > 65535) return web_err("网页服务: 端口无效");
    } else {
        /* 目录模式：端口参数可空，依次取 参数 → 网页.json 端口 → 默认 8080 */
        if (!dir || !dir[0]) return web_err("网页服务目录: 缺少目录");
        if (port <= 0 || port > 65535) {
            char *cfg_port = web_conf_value(dir, "端口", "8080");
            int p = cfg_port ? atoi(cfg_port) : 0;
            free(cfg_port);
            if (p > 0 && p <= 65535) port = p;
        }
    }

    pthread_mutex_lock(&s_web_lock);
    if (web_find_by_port(port)) {
        pthread_mutex_unlock(&s_web_lock);
        return web_err("网页服务: 端口已被占用");
    }
    WebSrv *w = web_alloc();
    if (!w) {
        pthread_mutex_unlock(&s_web_lock);
        return web_err("网页服务: 服务数量已达上限");
    }
    w->port = port;
    w->mode = mode;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        w->in_use = 0;
        pthread_mutex_unlock(&s_web_lock);
        return web_err("网页服务: 创建 socket 失败");
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
        w->in_use = 0;
        pthread_mutex_unlock(&s_web_lock);
        char b[128];
        snprintf(b, sizeof(b), "网页服务: 端口 %d 绑定失败", port);
        return web_err(b);
    }
    w->fd = fd;
    w->content = content ? strdup(content) : NULL;
    w->root = NULL;
    w->entry = NULL;
    if (mode == 1) {
        w->root = strdup(dir);
        w->entry = web_conf_value(dir, "入口", "index.html");
    }
    if (pthread_create(&w->thr, NULL, web_loop, w) != 0) {
        close(fd);
        free(w->content); w->content = NULL;
        free(w->root); free(w->entry);
        w->in_use = 0;
        pthread_mutex_unlock(&s_web_lock);
        return web_err("网页服务: 线程创建失败");
    }
    pthread_mutex_unlock(&s_web_lock);

    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    return vus_string_new(url);
}

/* AI_网页服务(端口, 内容) */
VusString *vus_web_serve(VusString *port, VusString *content) {
    if (!content || vus_string_len(content) == 0) return web_err("网页服务: 缺少内容");
    return web_start(0, vus_string_cstr(content), NULL,
                     port ? vus_string_cstr(port) : "");
}

/* AI_网页服务目录(目录, 端口) */
VusString *vus_web_serve_dir(VusString *dir, VusString *port) {
    return web_start(1, NULL, dir ? vus_string_cstr(dir) : "",
                     port ? vus_string_cstr(port) : "");
}

/* AI_网页停止(端口) */
VusString *vus_web_stop(VusString *port) {
    if (!port || vus_string_len(port) == 0) return vus_string_new("0");
    int p = atoi(vus_string_cstr(port));
    pthread_mutex_lock(&s_web_lock);
    WebSrv *w = web_find_by_port(p);
    if (!w) {
        pthread_mutex_unlock(&s_web_lock);
        return vus_string_new("0");
    }
    w->stop = 1;
    if (w->fd >= 0) shutdown(w->fd, SHUT_RDWR);
    pthread_join(w->thr, NULL);
    close(w->fd);
    free(w->content); w->content = NULL;
    free(w->root);  w->root = NULL;
    free(w->entry); w->entry = NULL;
    w->in_use = 0;
    pthread_mutex_unlock(&s_web_lock);
    return vus_string_new("1");
}