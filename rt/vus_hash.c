/*
 * vus_hash.c — VUS 哈希内建（SHA-256 FIPS 180-4 / MD5 RFC 1321，纯 C 实现）
 *
 * 脚本接口：
 *   SHA256(文本) → 64 位小写十六进制串（任意字节安全，按 VusString len 处理）
 *   MD5(文本)    → 32 位小写十六进制串
 *
 * 用于热更新补丁校验、文件完整性检查（配合 文件_读取 计算哈希）。
 */

#include "libvus_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================= SHA-256 ================= */
typedef struct {
    unsigned int state[8];
    unsigned long long total;
    unsigned char buf[64];
} VusSha256;

static const unsigned int SHA_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define SHA_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(VusSha256 *s, const unsigned char *p) {
    unsigned int w[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = SHA_ROR(w[i - 15], 7) ^ SHA_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = SHA_ROR(w[i - 2], 17) ^ SHA_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->state[0]; b = s->state[1]; c = s->state[2]; d = s->state[3];
    e = s->state[4]; f = s->state[5]; g = s->state[6]; h = s->state[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = SHA_ROR(e, 6) ^ SHA_ROR(e, 11) ^ SHA_ROR(e, 25);
        unsigned int ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + SHA_K[i] + w[i];
        unsigned int S0 = SHA_ROR(a, 2) ^ SHA_ROR(a, 13) ^ SHA_ROR(a, 22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->state[0] += a; s->state[1] += b; s->state[2] += c; s->state[3] += d;
    s->state[4] += e; s->state[5] += f; s->state[6] += g; s->state[7] += h;
}

static void sha256_init(VusSha256 *s) {
    s->state[0] = 0x6a09e667u; s->state[1] = 0xbb67ae85u;
    s->state[2] = 0x3c6ef372u; s->state[3] = 0xa54ff53au;
    s->state[4] = 0x510e527fu; s->state[5] = 0x9b05688cu;
    s->state[6] = 0x1f83d9abu; s->state[7] = 0x5be0cd19u;
    s->total = 0;
}

static void sha256_update(VusSha256 *s, const unsigned char *data, size_t len) {
    size_t fill = (size_t)s->total & 63;
    s->total += len;
    if (fill > 0) {
        size_t need = 64 - fill;
        if (len < need) { memcpy(s->buf + fill, data, len); return; }
        memcpy(s->buf + fill, data, need);
        sha256_block(s, s->buf);
        data += need;
        len -= need;
    }
    while (len >= 64) {
        sha256_block(s, data);
        data += 64;
        len -= 64;
    }
    if (len > 0) memcpy(s->buf, data, len);
}

static void sha256_final(VusSha256 *s, unsigned char out[32]) {
    unsigned long long bits = s->total * 8;
    size_t fill = (size_t)s->total & 63;
    unsigned char pad = 0x80;
    sha256_update(s, &pad, 1);
    unsigned char zero = 0;
    fill = ((size_t)s->total & 63);
    while (fill != 56) {
        sha256_update(s, &zero, 1);
        fill = (size_t)s->total & 63;
        if (fill == 56) break;
    }
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - i * 8));
    sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(s->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)s->state[i];
    }
}

/* ================= MD5 ================= */
typedef struct {
    unsigned int state[4];
    unsigned long long total;
    unsigned char buf[64];
} VusMd5;

static const unsigned char MD5_S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static const unsigned int MD5_K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

#define MD5_LROT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void md5_block(VusMd5 *m, const unsigned char *p) {
    unsigned int w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (unsigned int)p[i * 4] | ((unsigned int)p[i * 4 + 1] << 8) |
               ((unsigned int)p[i * 4 + 2] << 16) | ((unsigned int)p[i * 4 + 3] << 24);
    unsigned int a = m->state[0], b = m->state[1], c = m->state[2], d = m->state[3];
    for (int i = 0; i < 64; i++) {
        unsigned int f; int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        unsigned int oldd = d;
        d = c; c = b;
        b = b + MD5_LROT(a + f + MD5_K[i] + w[g], MD5_S[i]);
        a = oldd;
    }
    m->state[0] += a; m->state[1] += b; m->state[2] += c; m->state[3] += d;
}

static void md5_init(VusMd5 *m) {
    m->state[0] = 0x67452301u; m->state[1] = 0xefcdab89u;
    m->state[2] = 0x98badcfeu; m->state[3] = 0x10325476u;
    m->total = 0;
}

static void md5_update(VusMd5 *m, const unsigned char *data, size_t len) {
    size_t fill = (size_t)m->total & 63;
    m->total += len;
    if (fill > 0) {
        size_t need = 64 - fill;
        if (len < need) { memcpy(m->buf + fill, data, len); return; }
        memcpy(m->buf + fill, data, need);
        md5_block(m, m->buf);
        data += need; len -= need;
    }
    while (len >= 64) { md5_block(m, data); data += 64; len -= 64; }
    if (len > 0) memcpy(m->buf, data, len);
}

static void md5_final(VusMd5 *m, unsigned char out[16]) {
    unsigned long long bits = m->total * 8;
    unsigned char pad = 0x80;
    md5_update(m, &pad, 1);
    unsigned char zero = 0;
    while (((size_t)m->total & 63) != 56) md5_update(m, &zero, 1);
    for (int i = 0; i < 8; i++) { unsigned char lb = (unsigned char)(bits >> (8 * i)); md5_update(m, &lb, 1); }
    for (int i = 0; i < 4; i++) {
        out[i * 4] = (unsigned char)m->state[i];
        out[i * 4 + 1] = (unsigned char)(m->state[i] >> 8);
        out[i * 4 + 2] = (unsigned char)(m->state[i] >> 16);
        out[i * 4 + 3] = (unsigned char)(m->state[i] >> 24);
    }
}

/* ================= 脚本接口 ================= */
static const char HEX[] = "0123456789abcdef";

static VusString* hex_str(const unsigned char *dig, int n) {
    char *buf = (char*)malloc((size_t)n * 2 + 1);
    if (!buf) return vus_string_new("");
    for (int i = 0; i < n; i++) {
        buf[i * 2] = HEX[dig[i] >> 4];
        buf[i * 2 + 1] = HEX[dig[i] & 15];
    }
    buf[n * 2] = '\0';
    VusString *res = vus_string_new(buf);
    free(buf);
    return res;
}

VusString* vus_hash_sha256(VusString* data) {
    if (!data) return vus_string_new("");
    VusSha256 s;
    sha256_init(&s);
    sha256_update(&s, (const unsigned char*)data->data, (size_t)data->len);
    unsigned char dig[32];
    sha256_final(&s, dig);
    return hex_str(dig, 32);
}

VusString* vus_hash_md5(VusString* data) {
    if (!data) return vus_string_new("");
    VusMd5 m;
    md5_init(&m);
    md5_update(&m, (const unsigned char*)data->data, (size_t)data->len);
    unsigned char dig[16];
    md5_final(&m, dig);
    return hex_str(dig, 16);
}