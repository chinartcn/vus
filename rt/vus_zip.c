/*
 * vus_zip.c — VUS ZIP 内建（miniz，MIT 许可，vendored in rt/miniz/）
 *
 * 脚本接口（遵循 "0"/"-1" 约定）：
 *   解压_zip(zip路径, 目标目录)  → "0" 成功 / "-1" 失败（自动创建目录）
 *   压缩_zip(文件列表, zip路径)  → "0" 成功 / "-1" 失败（条目名取 basename）
 *
 * 内部接口（供 vus_vaz.c 替换 system unzip 使用）：
 *   int vus_zip_unzip_to_dir(const char* zippath, const char* dest);
 *
 * 安全：解包时拒绝绝对路径/路径穿越（zip-slip）条目；压缩侧条目名取 basename。
 */

#include "libvus_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "miniz/miniz.h"

/* 目录不存在则递归创建（mkdir -p 语义） */
static int mkdirs(const char *path) {
    if (!path || !*path) return -1;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { *p = '/'; return -1; }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* 条目名安全检查：拒绝空、绝对路径、包含 .. 路径穿越 */
static int zip_name_safe(const char *name) {
    if (!name || !*name) return 0;
    if (name[0] == '/' || name[0] == '\\') return 0;
    if (strstr(name, "..")) return 0;   /* 含 ".." 一律拒绝（含 "..x" 也保守拒绝） */
    return 1;
}

/* 从路径取 basename（最后一个 / 或 \ 之后的部分） */
static const char* path_basename(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

/* 解压 zip 到目录（核心实现，脚本/VAZ 共用） */
int vus_zip_unzip_to_dir(const char *zippath, const char *dest) {
    if (!zippath || !dest) return -1;
    if (mkdirs(dest) != 0) return -1;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zippath, 0)) return -1;

    int n = (int)mz_zip_reader_get_num_files(&zip);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { rc = -1; break; }
        if (st.m_is_directory) continue;
        if (!zip_name_safe(st.m_filename)) { rc = -1; break; }   /* zip-slip */
        /* 释放到内存再落盘，保证路径完全由 dest 拼接 */
        size_t sz = 0;
        void *buf = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!buf) { rc = -1; break; }
        char out[1200];
        snprintf(out, sizeof(out), "%s/%s", dest, st.m_filename);
        {   /* 仅创建条目所在父目录（避免把文件名当目录 mkdir） */
            char parent[1200];
            snprintf(parent, sizeof(parent), "%s", out);
            char *slash = strrchr(parent, '/');
            if (slash && slash != parent) *slash = '\0';
            if (mkdirs(parent) != 0) { mz_free(buf); rc = -1; break; }
        }
        FILE *fp = fopen(out, "wb");
        if (!fp) { mz_free(buf); rc = -1; break; }
        size_t wr = fwrite(buf, 1, sz, fp);
        fclose(fp);
        mz_free(buf);
        if (wr != sz) { rc = -1; break; }
    }
    mz_zip_reader_end(&zip);
    return rc;
}

/* 脚本：解压_zip(zip路径, 目标目录) → "0"/"-1" */
VusString* vus_zip_unzip(VusString* zippath, VusString* dest) {
    if (!zippath || !dest || !zippath->data || !dest->data)
        return vus_string_new("-1");
    int r = vus_zip_unzip_to_dir(zippath->data, dest->data);
    return vus_string_new(r == 0 ? "0" : "-1");
}

/* 脚本：压缩_zip(文件列表, zip路径) → "0"/"-1"（条目名取 basename）。
 * listobj 为 VUS 列表对象（VusObject(TYPE_LIST) 或裸 VusList*），由
 * vus_list_unwrap 解包；元素为字符串（文件路径）。 */
VusString* vus_zip_compress_list(void* listobj, VusString* zippath) {
    if (!zippath || !zippath->data) return vus_string_new("-1");
    VusList *list = vus_list_unwrap(listobj);
    if (!list || vus_list_len(list) <= 0) return vus_string_new("-1");

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, zippath->data, 0)) return vus_string_new("-1");

    int n = vus_list_len(list);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        VusString *p = (VusString*)vus_list_get(list, i);
        if (!p) continue;
        FILE *fp = fopen(p->data, "rb");
        if (!fp) { rc = -1; break; }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        rewind(fp);
        void *buf = NULL;
        if (sz > 0) {
            buf = malloc((size_t)sz);
            if (!buf) { fclose(fp); rc = -1; break; }
            if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
                free(buf); fclose(fp); rc = -1; break;
            }
        }
        fclose(fp);
        const char *name = path_basename(p->data);
        if (!mz_zip_writer_add_mem(&zip, name, buf ? buf : "", (size_t)(sz > 0 ? sz : 0), MZ_BEST_SPEED)) {
            if (buf) free(buf);
            rc = -1; break;
        }
        if (buf) free(buf);
    }
    if (rc != 0 || !mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        if (rc == 0) rc = -1;
        return vus_string_new("-1");
    }
    mz_zip_writer_end(&zip);
    return vus_string_new("0");
}