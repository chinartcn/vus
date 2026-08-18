/*
 * VUS XYZ 体感音游 —— vus chart 子命令：谱面生成器
 *
 * 用法：vus chart <audio> [-o chart.json]
 *
 * 输入：
 *   .wav （PCM）      —— 直接解析（零依赖）
 *   .mp3 及其他       —— 若存在 ffmpeg 则转临时 wav 后解析（按需调用，Termux 已具备）
 * 输出：
 *   chart.json —— {title, bpm, duration_ms, notes:[{t(ms), axis(Z/X/Y), target(milli-g)}]}
 *
 * 算法（对应设计文档 §7）：
 *   1) 分帧 + 加窗 + radix-2 FFT → 频带能量（低频/中频/高频）
 *   2) 包络（低频+中频+高频）峰值检测 → 卡点时刻 t
 *   3) 主频带决定轴（低频→Z、中频→X、高频→Y）
 *   4) target = 归一化能量 → milli-g（期望玩家甩出的目标值，与传感器同单位）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>

static const char* g_input = NULL;
static const char* g_output = NULL;
static double g_sr = 44100;
static int g_duration_ms = 0;

/* ==================== 简易 radix-2 FFT ==================== */
typedef struct { double re, im; } cplx;

static void fft(cplx* a, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { cplx t = a[i]; a[i] = a[j]; a[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        cplx wl = { cos(ang), sin(ang) };
        for (int i = 0; i < n; i += len) {
            cplx w = { 1.0, 0.0 };
            for (int j = 0; j < len / 2; j++) {
                cplx u = a[i + j];
                cplx v = { a[i + j + len / 2].re * w.re - a[i + j + len / 2].im * w.im,
                           a[i + j + len / 2].re * w.im + a[i + j + len / 2].im * w.re };
                a[i + j].re = u.re + v.re; a[i + j].im = u.im + v.im;
                a[i + j + len / 2].re = u.re - v.re; a[i + j + len / 2].im = u.im - v.im;
                cplx t2 = { w.re * wl.re - w.im * wl.im, w.re * wl.im + w.im * wl.re };
                w = t2;
            }
        }
    }
}

/* ==================== WAV 解析（16-bit PCM，单/双声道） ==================== */
typedef struct {
    double* samples;   /* 单声道归一化到 [-1,1] */
    int count;
} Pcm;

static int wav_load(const char* path, Pcm* out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return -1;
    }
    short fmt = 0;
    unsigned short ch = 0;
    unsigned int sr = 0;
    unsigned short bits = 0;
    int have_data = 0;
    for (;;) {
        char id[4]; unsigned int sz;
        if (fread(id, 1, 4, f) != 4) break;
        if (fread(&sz, 4, 1, f) != 1) break;
        if (memcmp(id, "fmt ", 4) == 0) {
            unsigned char b[16];
            size_t n = sz < 16 ? sz : 16;
            if (fread(b, 1, n, f) != n) break;
            memcpy(&fmt, b, 2);
            memcpy(&ch, b + 2, 2);
            memcpy(&sr, b + 4, 4);
            memcpy(&bits, b + 14, 2);
            if (sz > n) fseek(f, sz - n, SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            long data_len = sz;
            long nframes = (bits == 0) ? 0 : (data_len / (bits / 8) / ch);
            out->count = (int)nframes;
            out->samples = (double*)malloc(sizeof(double) * (size_t)nframes);
            if (!out->samples) { fclose(f); return -1; }
            int idx = 0;
            double scale = (bits == 16) ? (1.0 / 32768.0) : (1.0 / 128.0);
            for (long i = 0; i < nframes; i++) {
                double v = 0;
                for (unsigned short c = 0; c < ch; c++) {
                    if (bits == 16) { int16_t s; if (fread(&s, 2, 1, f) != 1) { v = 0; continue; } v += (double)s * scale; }
                    else { int8_t s; if (fread(&s, 1, 1, f) != 1) { v = 0; continue; } v += (double)s * scale; }
                }
                out->samples[idx++] = v / ch;
            }
            have_data = 1;
            break;
        } else {
            fseek(f, sz, SEEK_CUR);
        }
    }
    fclose(f);
    if (!have_data || fmt != 1) { free(out->samples); out->samples = NULL; return -1; }
    g_sr = (double)sr;
    g_duration_ms = (int)((double)out->count * 1000.0 / g_sr);
    return 0;
}

/* ==================== 谱面生成 ==================== */
static int create_chart(const Pcm* pcm, FILE* out, int* out_nnotes)
{
    const int N = 1024;          /* FFT 窗（2^10） */
    const int HOP = 448;         /* 滑步（~10ms/帧） */
    int nframes = pcm->count / HOP + 1;

    double* onset = (double*)calloc((size_t)nframes, sizeof(double));
    double* bandL = (double*)calloc((size_t)nframes, sizeof(double));
    double* bandM = (double*)calloc((size_t)nframes, sizeof(double));
    double* bandH = (double*)calloc((size_t)nframes, sizeof(double));
    if (!onset || !bandL || !bandM || !bandH) return -1;

    cplx* buf = (cplx*)malloc(sizeof(cplx) * (size_t)N);
    double* win = (double*)malloc(sizeof(double) * (size_t)N);
    for (int i = 0; i < N; i++) win[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (N - 1))); /* 汉宁窗 */

    double max_onset = 0;
    for (int f = 0; f < nframes; f++) {
        int start = f * HOP;
        memset(buf, 0, sizeof(cplx) * (size_t)N);
        for (int i = 0; i < N; i++) {
            int s = start + i;
            double v = (s < pcm->count) ? pcm->samples[s] * win[i] : 0.0;
            buf[i].re = v;
        }
        fft(buf, N);
        /* 频带能量：bin 频率 = k*g_sr/N */
        double l = 0, m = 0, h = 0;
        for (int i = 1; i <= N / 2; i++) {
            double freq = (double)i * g_sr / N;
            double mag2 = buf[i].re * buf[i].re + buf[i].im * buf[i].im;
            if (freq >= 20 && freq < 200) l += mag2;
            else if (freq >= 200 && freq < 1000) m += mag2;
            else if (freq >= 1000 && freq < 5000) h += mag2;
        }
        bandL[f] = l; bandM[f] = m; bandH[f] = h;
        onset[f] = l + m + h;
        if (onset[f] > max_onset) max_onset = onset[f];
    }

    /* 平滑包络（简单移动平均，半径 5 帧） */
    double* smooth = (double*)calloc((size_t)nframes, sizeof(double));
    for (int f = 0; f < nframes; f++) {
        double acc = 0; int cnt = 0;
        for (int d = -5; d <= 5; d++) {
            int s = f + d;
            if (s >= 0 && s < nframes) { acc += onset[s]; cnt++; }
        }
        smooth[f] = acc / cnt;
    }

    /* 峰检测：局部极大 + 高于自适应阈值（mean + 0.5*std），最小间距 250ms */
    double mean = 0;
    for (int f = 0; f < nframes; f++) mean += smooth[f];
    mean /= nframes;
    double var = 0;
    for (int f = 0; f < nframes; f++) { double d = smooth[f] - mean; var += d * d; }
    var /= nframes;
    double thresh = mean + 0.5 * sqrt(var);
    if (thresh <= 0) thresh = mean;
    if (max_onset > 0) thresh = thresh < max_onset * 0.25 ? thresh : mean;

    const int MIN_GAP_MS = 250;
    int last_t = -MIN_GAP_MS;

    /* 先扫一遍收集峰时间戳用于估 bpm（中值间距），再写 JSON。
       第一遍：收集到临时数组。 */
    typedef struct { int t; char axis; int d; int target; } Note;
    Note* notes = (Note*)malloc(sizeof(Note) * (size_t)nframes);
    int nnotes = 0;
    for (int f = 2; f < nframes - 2; f++) {
        if (smooth[f] >= smooth[f-1] && smooth[f] >= smooth[f+1] &&
            smooth[f] >= smooth[f-2] && smooth[f] >= smooth[f+2] && smooth[f] > thresh) {
            int t = (int)(((double)(f * HOP) / g_sr) * 1000.0);
            if (t - last_t >= MIN_GAP_MS) {
                /* 主频带决定轴 */
                char axis = 'Z';
                int d = 0;
                double dom = bandL[f];
                if (bandM[f] > dom) { dom = bandM[f]; axis = 'X'; d = 1; }
                if (bandH[f] > dom) { axis = 'Y'; d = 2; }
                /* target：归一化能量 → milli-g，限定 [600,2000]（0.6g~2g） */
                double norm = (max_onset > 0) ? (onset[f] / max_onset) : 0.1;
                double tg = norm * 1800.0;
                if (tg < 600) tg = 600;
                if (tg > 2000) tg = 2000;
                notes[nnotes].t = t;
                notes[nnotes].axis = axis;
                notes[nnotes].d = d;
                notes[nnotes].target = (int)tg;
                nnotes++;
                last_t = t;
            }
        }
    }

    /* bpm：间距中值估 */
    int* gaps = (int*)malloc(sizeof(int) * (nnotes > 0 ? nnotes : 1));
    int ng = 0;
    for (int i = 1; i < nnotes; i++) gaps[ng++] = notes[i].t - notes[i-1].t;
    int bpm = 0;
    if (ng > 0) {
        int med = gaps[ng / 2];
        if (med > 0) bpm = (int)(60000.0 / med + 0.5);
        if (bpm < 40) bpm = 0;
    }

    /* 写 JSON */
    int first = 1;
    fprintf(out, "{\n");
    fprintf(out, "  \"title\": \"%s\",\n", g_input);
    fprintf(out, "  \"bpm\": %d,\n", bpm);
    fprintf(out, "  \"duration_ms\": %d,\n", g_duration_ms);
    fprintf(out, "  \"note_count\": %d,\n", nnotes);
    fprintf(out, "  \"notes\": [\n");
    for (int i = 0; i < nnotes; i++) {
        fprintf(out, "    %s{\"t\": %d, \"axis\": \"%c\", \"d\": %d, \"target\": %d}",
                first ? " " : ",",
                notes[i].t, notes[i].axis, notes[i].d, notes[i].target);
        first = 0;
        fprintf(out, "\n");
    }
    fprintf(out, "  ]\n}\n");

    if (out_nnotes) *out_nnotes = nnotes;
    free(notes); free(gaps); free(smooth);
    free(buf); free(win);
    free(onset); free(bandL); free(bandM); free(bandH);
    return 0;
}

/* ==================== 入口 ==================== */
int vus_chart_main(int argc, char* argv[])
{
    g_input = NULL; g_output = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { g_output = argv[++i]; }
        else if (argv[i][0] != '-') {
            if (!g_input) g_input = argv[i];
        }
    }
    if (!g_input) {
        fprintf(stderr, "用法: vus chart <音频> [-o chart.json]\n");
        return 1;
    }
    if (!g_output) { g_output = g_input; }

    /* mp3 等其他格式：若可用 ffmpeg 则转临时 wav */
    char tmp[4096] = {0};
    const char* src = g_input;
    if (strstr(g_input, ".wav") == NULL && strstr(g_input, ".WAV") == NULL) {
        snprintf(tmp, sizeof(tmp), "/tmp/vus_chart_%ld.wav", (long)getpid());
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "ffmpeg -y -i \"%s\" -f wav \"%s\" >/dev/null 2>&1", g_input, tmp);
        if (system(cmd) != 0) {
            fprintf(stderr, "错误: 输入不是 WAV 且 ffmpeg 转换失败（请装 ffmpeg: pkg install ffmpeg）\n");
            return 1;
        }
        src = tmp;
    }

    Pcm pcm = {0};
    if (wav_load(src, &pcm) != 0) {
        fprintf(stderr, "错误: 无法解析 WAV: %s\n", src);
        if (tmp[0]) remove(tmp);
        return 1;
    }

    char outbuf[4096];
    if (strstr(g_output, ".json") == NULL) {
        /* 缺省输出名：把输入扩展名替换为 .chart.json（如 song.mp3 → song.chart.json） */
        snprintf(outbuf, sizeof(outbuf), "%s", g_output);
        char* dot = strrchr(outbuf, '.');
        if (dot && dot > outbuf) *dot = '\0';
        size_t avail = sizeof(outbuf) - strlen(outbuf) - 1;
        strncat(outbuf, ".chart.json", avail);
        g_output = outbuf;
    }

    FILE* fo = fopen(g_output, "w");
    if (!fo) {
        fprintf(stderr, "错误: 无法写入输出: %s\n", g_output);
        if (tmp[0]) remove(tmp);
        free(pcm.samples);
        return 1;
    }
    int created_notes = 0;
    int rc = create_chart(&pcm, fo, &created_notes);
    fclose(fo);
    if (tmp[0]) remove(tmp);
    free(pcm.samples);
    if (rc != 0) { fprintf(stderr, "错误: 谱面生成失败\n"); return 1; }
    fprintf(stderr, "已生成谱面: %s (%d notes)\n", g_output, created_notes);
    return 0;
}