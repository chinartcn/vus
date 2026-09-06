/*
 * Theme.java — 全局主题常量（单一真源）
 *
 * 历史：VuaRenderer 内散落硬编码色值且与 styles.xml 双处维护（注释声称"一致"）。
 * 现收敛到本类：浅色/暗色两套取色，控件构建器按 dark 标志读取，样式文件仅保留
 * 系统级属性的兜底；改主题只动这里。
 */
package com.vus.android;

final class Theme {

    // 基础色（与 styles.xml colorAccent 保持一致的唯一来源）
    static final int PRIMARY      = 0xFF2962FF;  // 主题主色（按钮/标题/聚焦描边）
    static final int BG_LIGHT     = 0xFFF5F6FA;  // 浅色页面浅灰底（Material Light windowBackground）
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
    static final int ACCENT_DARK  = 0xFF8FB4FF;  // 暗色主题下的主色亮化（比 PRIMARY 更醒目）
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

    private Theme() { }

    static int card(boolean dark)       { return dark ? CARD_DARK   : CARD_LIGHT; }
    static int text(boolean dark)       { return dark ? TEXT_DARK   : TEXT_LIGHT; }
    static int sub(boolean dark)        { return dark ? SUB_DARK    : SUB_LIGHT; }
    static int muted(boolean dark)      { return dark ? MUTED_DARK  : MUTED_LIGHT; }
    static int accent(boolean dark)     { return dark ? ACCENT_DARK : PRIMARY; }
    static int field(boolean dark)      { return dark ? FIELD_DARK  : FIELD_LIGHT; }
    static int stroke(boolean dark)     { return dark ? FIELD_STROKE_DARK : FIELD_STROKE_LIGHT; }
    static int rowBg(boolean dark)      { return dark ? ROW_DARK    : 0xFFFFFFFF; }
    static int divider(boolean dark)    { return dark ? DIV_DARK    : DIV_LIGHT; }
}