/*
 * Theme.java — 全局主题（单一真源）
 *
 * MD3 风格化（2026-09）：活力主色运行期可变（主题_主色），并按官方色彩映射
 * （material-components Color.md）反射采样 Android 12+ 的系统动态色
 * （system_accent1_*），让 12+ 跟随壁纸、10/11 用手动色。深浅两套取色，
 * 控件构建器按 dark 标志读取；改主题只动这里。
 */
package com.vus.android;

final class Theme {

    // 基础色
    static final int PRIMARY      = 0xFF2962FF;  // 默认主色（主题_主色("默认") 复位目标）
    static final int BG_LIGHT     = 0xFFF5F6FA;  // 浅色页面浅灰底
    static final int BG_DARK      = 0xFF121212;  // 暗色页面背景

    // 卡片 / 文本 / 辅助色（浅色、暗色两套，按 dark 标志取用）
    static final int CARD_LIGHT   = 0xFFFFFFFF;
    static final int CARD_DARK    = 0xFF1E1E1E;
    static final int TEXT_LIGHT   = 0xFF1A1F2E;
    static final int TEXT_DARK    = 0xFFE0E0E0;
    static final int SUB_LIGHT    = 0xFF6B7280;  // 列表副标题
    static final int SUB_DARK     = 0xFF9A9A9A;
    static final int MUTED_LIGHT  = 0xFF666666;  // 占位/次要点缀
    static final int MUTED_DARK   = 0xFF888888;
    static final int FIELD_LIGHT  = 0xFFF2F3F7;  // 输入框衬底
    static final int FIELD_DARK   = 0xFF2A2A2A;
    static final int FIELD_STROKE_LIGHT = 0xFFDDE2EC;
    static final int FIELD_STROKE_DARK  = 0xFF444444;
    static final int ROW_DARK     = 0xFF1E1E1E;  // 列表/表格容器底
    static final int ROW_ALT_LIGHT = 0xFFF5F5F5; // 表格隔行色
    static final int ROW_ALT_DARK  = 0xFF2A2A2A;
    static final int ROW_HDR_LIGHT = 0xFFE0E0E0; // 表格表头底色
    static final int ROW_HDR_DARK  = 0xFF333333;
    static final int ROW_EVEN_LIGHT = 0xFFEEEEEE;
    static final int ROW_EVEN_DARK  = 0xFF242424;
    static final int DIV_LIGHT     = 0x14000000; // 分隔线（浅）
    static final int DIV_DARK      = 0x26FFFFFF; // 分隔线（暗）

    // MD3 primaryContainer 回退基准（官方映射 system_accent1_100 / _700 失败时用）
    static final int PC_LIGHT = 0xFFDDE4FF;
    static final int PC_DARK  = 0xFF24309F;

    /** 运行期主色（主题_主色 写入；动态取色/手动色共用）。默认 PRIMARY。 */
    static volatile int sPrimary = PRIMARY;

    private Theme() { }

    /* ---- 运行期主色控制 ---- */

    static void setPrimary(int c) { if (c != 0) sPrimary = c; }

    /** 主色亮化（暗色主题下做文字/强调用，避免深色底上对比不足）。 */
    static int brighten(int c) {
        int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        r = (int) (r * 0.35f + 255 * 0.65f);
        g = (int) (g * 0.35f + 255 * 0.65f);
        b = (int) (b * 0.35f + 255 * 0.65f);
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    /** 颜色字符串 → int（"0xRRGGBB" / "#RRGGBB" / "#AARRGGBB" / 系统色名），失败/空回退 def。 */
    static int parseColor(String s, int def) {
        if (s == null || s.isEmpty()) return def;
        try {
            if (s.startsWith("0x") || s.startsWith("0X")) {
                return 0xFF000000 | (int) Long.parseLong(s.substring(2), 16);
            }
            return android.graphics.Color.parseColor(s);
        } catch (Throwable t) {
            return def;
        }
    }

    /* ---- Android 12+ 动态取色（反射系统动态资源；官方映射：primary=accent1_600） ---- */

    private static Integer sDynamicPrimary;   // 缓存：壁纸主色（取不到=null）

    /** 反射采样系统动态色；非 Android 12+ 或资源缺失返回 def。 */
    private static int systemColor(String resName, int def) {
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            try {
                android.content.res.Resources res = android.content.res.Resources.getSystem();
                int id = res.getIdentifier(resName, "color", "android");
                if (id != 0) return res.getColor(id);
            } catch (Throwable ignored) { }
        }
        return def;
    }

    /** 壁纸动态主色（缓存）；Android 12+ 取到即返回，否则 PRIMARY。 */
    static int dynamicPrimary() {
        if (sDynamicPrimary == null) {
            sDynamicPrimary = systemColor("system_accent1_500", PRIMARY);
        }
        return sDynamicPrimary;
    }

    /* ---- 语义色（按 dark 取用；主色相关跟随 sPrimary / 动态色） ---- */

    static int card(boolean dark)       { return dark ? CARD_DARK   : CARD_LIGHT; }
    static int text(boolean dark)       { return dark ? TEXT_DARK   : TEXT_LIGHT; }
    static int sub(boolean dark)        { return dark ? SUB_DARK    : SUB_LIGHT; }
    static int muted(boolean dark)      { return dark ? MUTED_DARK  : MUTED_LIGHT; }
    /** 强调/主控着色：浅色=主色，暗色=亮化主色（保证对比）。 */
    static int accent(boolean dark)     { return dark ? brighten(sPrimary) : sPrimary; }
    static int field(boolean dark)      { return dark ? FIELD_DARK  : FIELD_LIGHT; }
    static int stroke(boolean dark)     { return dark ? FIELD_STROKE_DARK : FIELD_STROKE_LIGHT; }
    static int rowBg(boolean dark)      { return dark ? ROW_DARK    : 0xFFFFFFFF; }
    static int divider(boolean dark)    { return dark ? DIV_DARK    : DIV_LIGHT; }

    /** MD3 primaryContainer：按钮填充/选中衬底（12+ 动态色，10/11 回退系谱色）。
     * 暗色用主色底 + 亮字，比亮化主色更贴 MD3。 */
    static int primaryContainer(boolean dark) {
        return dark ? systemColor("system_accent1_700", PC_DARK)
                    : systemColor("system_accent1_100", PC_LIGHT);
    }

    /** MD3 onPrimary：主色/容器底色块上的文字。浅=白，暗=亮容器文字色。 */
    static int onPrimary(boolean dark) {
        return dark ? systemColor("system_accent1_800", 0xFFDDE4FF)
                    : systemColor("system_accent1_0", 0xFFFFFFFF);
    }
}