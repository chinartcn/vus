/*
 * vus_img.c — VUS 图片导出内建（stb_image_write，public domain/MIT，vendored in rt/stb/）
 *
 * 脚本接口（遵循 "0"/"-1" 约定，像素数据为 VusString 原始字节缓冲）：
 *   图片_保存PNG(路径, 宽, 高, 像素数据[, 通道数=4])              → "0"/"-1"
 *   图片_保存JPG(路径, 宽, 高, 像素数据, 质量[, 通道数=3])        → "0"/"-1"
 *   图片_保存BMP(路径, 宽, 高, 像素数据[, 通道数=4])              → "0"/"-1"
 * 通道数 3=RGB / 4=RGBA；JPEG 质量 1~100。
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb/stb_image_write.h"

#include "libvus_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 校验宽高与数据长度的匹配 */
static int img_args_ok(int w, int h, int comp, int64_t datalen) {
    if (w <= 0 || h <= 0) return 0;
    if (comp != 3 && comp != 4) return 0;
    return datalen == (int64_t)w * h * comp;
}

/* 图片_保存PNG */
VusString* vus_img_png_save(VusString* path, int w, int h, VusString* data, int comp) {
    if (!path || !data) return vus_string_new("-1");
    if (!img_args_ok(w, h, comp, (int64_t)data->len)) return vus_string_new("-1");
    int r = stbi_write_png(path->data, w, h, comp, data->data, w * comp);
    return vus_string_new(r ? "0" : "-1");
}

/* 图片_保存JPG */
VusString* vus_img_jpg_save(VusString* path, int w, int h, VusString* data, int quality, int comp) {
    if (!path || !data) return vus_string_new("-1");
    if (!img_args_ok(w, h, comp, (int64_t)data->len)) return vus_string_new("-1");
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int r = stbi_write_jpg(path->data, w, h, comp, data->data, quality);
    return vus_string_new(r ? "0" : "-1");
}

/* 图片_保存BMP */
VusString* vus_img_bmp_save(VusString* path, int w, int h, VusString* data, int comp) {
    if (!path || !data) return vus_string_new("-1");
    if (!img_args_ok(w, h, comp, (int64_t)data->len)) return vus_string_new("-1");
    int r = stbi_write_bmp(path->data, w, h, comp, data->data);
    return vus_string_new(r ? "0" : "-1");
}