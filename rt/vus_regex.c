/*
 * vus_regex.c — VUS 正则内建（Oniguruma 6.9.9，BSD-2 许可，vendored in rt/oniguruma/）
 *
 * 脚本接口（VusString* 进出，遵循既有 "0"/"-1" 约定）：
 *   正则_查找(文本, 模式)       → "1"/"0"
 *   正则_匹配(文本, 模式)       → 第一个完整匹配串；无匹配/错误返回空串
 *   正则_替换(文本, 模式, 替换) → 全部替换；替换串支持 \0~\9 引用匹配分组
 *
 * 实现：UTF-8 编码；每次调用编译一次；region 偏移基于 onig_search 的 str 起点；
 * Oniguruma 编译/执行不共享可变全局，线程安全。
 */

#include "libvus_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oniguruma/oniguruma.h"

#define RE_OPTION ONIG_OPTION_NONE

/* 编译模式；失败返回 0 并置 reg=NULL */
static int re_compile(const char *pat, int plen, regex_t **reg) {
    OnigErrorInfo einfo;
    int r = onig_new(reg, (const UChar*)pat, (const UChar*)(pat + plen),
                     RE_OPTION, ONIG_ENCODING_UTF8, ONIG_SYNTAX_DEFAULT, &einfo);
    return (r == ONIG_NORMAL) ? 0 : -1;
}

/* 正则_查找：模式能否在文本中找到 */
VusString* vus_regex_find(VusString* text, VusString* pattern) {
    if (!text || !pattern || !text->data || !pattern->data) return vus_string_new("0");
    regex_t *reg;
    if (re_compile(pattern->data, pattern->len, &reg) != 0) return vus_string_new("0");
    OnigRegion *rg = onig_region_new();
    int r = onig_search(reg, (const UChar*)text->data,
                        (const UChar*)(text->data + text->len),
                        (const UChar*)text->data,
                        (const UChar*)(text->data + text->len),
                        rg, RE_OPTION);
    onig_region_free(rg, 1);
    onig_free(reg);
    return vus_string_new(r >= 0 ? "1" : "0");
}

/* 正则_匹配：返回第一个完整匹配串 */
VusString* vus_regex_match(VusString* text, VusString* pattern) {
    if (!text || !pattern || !text->data || !pattern->data) return vus_string_new("");
    regex_t *reg;
    if (re_compile(pattern->data, pattern->len, &reg) != 0) return vus_string_new("");
    OnigRegion *rg = onig_region_new();
    int r = onig_search(reg, (const UChar*)text->data,
                        (const UChar*)(text->data + text->len),
                        (const UChar*)text->data,
                        (const UChar*)(text->data + text->len),
                        rg, RE_OPTION);
    if (r < 0 || rg->num_regs < 1 ||
        rg->beg[0] < 0 || rg->end[0] < rg->beg[0]) {
        onig_region_free(rg, 1);
        onig_free(reg);
        return vus_string_new("");
    }
    VusString *res = vus_string_new_len(text->data + rg->beg[0],
                                        rg->end[0] - rg->beg[0]);
    onig_region_free(rg, 1);
    onig_free(reg);
    return res;
}

/* 正则_替换：全部替换；替换串 \0~\9 展开为对应分组（\0 = 整个匹配） */
VusString* vus_regex_replace(VusString* text, VusString* pattern, VusString* repl) {
    if (!text || !pattern || !repl) return vus_string_new("");
    if (!text->data || !pattern->data || !repl->data) return vus_string_new("");
    regex_t *reg;
    if (re_compile(pattern->data, pattern->len, &reg) != 0) return vus_string_new("");

    const UChar *str = (const UChar*)text->data;
    const UChar *str_end = (const UChar*)(text->data + text->len);
    const char *rp = repl->data;

    size_t cap = (size_t)(text->len * 2 + 64);
    char *out = (char*)malloc(cap + 1);
    if (!out) { onig_free(reg); return vus_string_new(""); }
    size_t opos = 0;

    OnigRegion *rg = onig_region_new();
    const UChar *pos = str;
    while (pos < str_end) {
        onig_region_clear(rg);
        int r = onig_search(reg, str, str_end, pos, str_end, rg, RE_OPTION);
        if (r < 0) {   /* 无更多匹配：拷贝剩余原文 */
            size_t rest = (size_t)(str_end - pos);
            if (opos + rest + 1 > cap) { cap = (opos + rest + 1) * 2; out = realloc(out, cap + 1); if (!out) { out = NULL; break; } }
            memcpy(out + opos, pos, rest); opos += rest;
            break;
        }
        /* 匹配前的原文 */
        size_t pre = (size_t)(rg->beg[0] - (pos - str));
        if (opos + pre + 1 > cap) { cap = (opos + pre + 1) * 2; out = realloc(out, cap + 1); if (!out) break; }
        memcpy(out + opos, pos, pre); opos += pre;
        /* 展开替换串 */
        for (const char *q = rp; *q; q++) {
            if (*q == '\\' && q[1] >= '0' && q[1] <= '9') {
                int gi = q[1] - '0';
                q++;
                if (gi < rg->num_regs && rg->beg[gi] >= 0 && rg->end[gi] >= rg->beg[gi]) {
                    size_t mlen = (size_t)(rg->end[gi] - rg->beg[gi]);
                    if (mlen > 0) {
                        if (opos + mlen + 1 > cap) { cap = (opos + mlen + 1) * 2; out = realloc(out, cap + 1); if (!out) break; }
                        memcpy(out + opos, str + rg->beg[gi], mlen); opos += mlen;
                    }
                }
            } else {
                if (opos + 2 > cap) { cap = cap * 2 + 64; out = realloc(out, cap + 1); if (!out) break; }
                out[opos++] = *q;
            }
        }
        /* 推进；空匹配时至少前进一个字节，避免死循环 */
        size_t mlen = (size_t)(rg->end[0] - rg->beg[0]);
        const UChar *npos = str + rg->end[0];
        if (mlen == 0) {
            if (npos >= str_end) break;
            if (opos + 1 > cap) { cap = cap * 2 + 64; out = realloc(out, cap + 1); if (!out) break; }
            out[opos++] = (char)*npos;
            npos++;
        }
        if (npos < pos) npos = pos + 1;   /* 防御：禁止回退 */
        pos = npos;
        if (out == NULL) break;
    }
    onig_region_free(rg, 1);
    onig_free(reg);
    if (out == NULL) { free(out); return vus_string_new(""); }
    out[opos] = '\0';
    VusString *res = vus_string_new_len(out, (int)opos);
    free(out);
    return res;
}