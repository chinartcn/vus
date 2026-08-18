#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
vus_export.py —— 设计 JSON → .vus 源码导出器（纯 Python 标准库实现）

把 HTML 高级排版设计器（gui-designer）生成的"设计数据 JSON"转换成 VUS 语言的
图形界面源码字符串，供后续由 VUS 编译器/运行时编译执行。

用法：
    # 作为模块调用
    from vus_export import export_to_vus
    vus_src = export_to_vus(design_dict)

    # 命令行：把输入 JSON 转成 .vus 文件（缺省输出到标准输出）
    python3 vus_export.py <input.json> [output.vus]
"""

import json
import sys

# 控件 type → 对应的 VUS 内建绘图/控件函数名
TYPE_TO_FUNC = {
    "label":       "图形_文字",
    "button":      "图形_按钮",
    "slider":      "图形_滑块",
    "switch":      "图形_开关",
    "spin":        "图形_微调",
    "radio":       "图形_单选",
    "round_rect":  "图形_圆角矩形",
    "fill_rect":   "图形_填充",
    "circle":      "图形_画圆",
    "fill_circle": "图形_填充圆",
    "arc":         "图形_圆弧",
    "progress":    "图形_进度",
    "textbox":     "图形_文本框",
}


def hex_color(value):
    """颜色统一输出为 VUS 的 0x%06X 字面量。"""
    return "0x%06X" % int(value)


def qstr(value):
    """把数值/文本变成 VUS 字符串字面量，正确处理引号、反斜杠与换行。"""
    text = str(value)
    text = text.replace("\\", "\\\\")
    text = text.replace('"', '\\"')
    text = text.replace("\n", "\\n")
    return '"%s"' % text


def qnum(value):
    """数值字面量：保留整数/浮点形式，布尔直接转 0/1。"""
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def _emit_control(c):
    """根据控件 dict 生成一行（或一行注释+调用）的 VUS 源码。"""
    func = TYPE_TO_FUNC.get(c.get("type", ""))
    if func is None:
        return "# 未知控件类型：%s（已忽略）" % qstr(c.get("type", "")).replace('"', "")
    t = c.get("type")

    if t == "label":
        args = "%d, %d, %s, %s" % (
            int(c.get("x", 0)), int(c.get("y", 0)),
            qstr(c.get("text", "")), hex_color(c.get("color", 0x000000)),
        )
    elif t == "button":
        args = "%s, %d, %d, %d, %d, %s" % (
            qstr(c.get("name", "")),
            int(c.get("x", 0)), int(c.get("y", 0)),
            int(c.get("w", 0)), int(c.get("h", 0)),
            qstr(c.get("text", "")),
        )
    elif t == "slider":
        args = "%s, %d, %d, %d, %s, %s, %s" % (
            qstr(c.get("name", "")),
            int(c.get("x", 0)), int(c.get("y", 0)), int(c.get("w", 0)),
            qnum(c.get("value", 0)), qnum(c.get("min", 0)), qnum(c.get("max", 100)),
        )
    elif t == "switch":
        args = "%s, %d, %d, %s" % (
            qstr(c.get("name", "")),
            int(c.get("x", 0)), int(c.get("y", 0)),
            qnum(c.get("on", False)),
        )
    elif t == "spin":
        args = "%s, %d, %d, %s, %s" % (
            qstr(c.get("name", "")),
            int(c.get("x", 0)), int(c.get("y", 0)),
            qnum(c.get("value", 0)), qnum(c.get("step", 1)),
        )
    elif t == "radio":
        args = "%s, %d, %d, %d, %s, %s" % (
            qstr(c.get("name", "")),
            int(c.get("x", 0)), int(c.get("y", 0)), int(c.get("item_h", 20)),
            qstr(c.get("options", "")), qnum(c.get("sel", 0)),
        )
    elif t == "round_rect":
        args = "%d, %d, %d, %d, %d, %s" % (
            int(c.get("x", 0)), int(c.get("y", 0)),
            int(c.get("w", 0)), int(c.get("h", 0)),
            int(c.get("radius", 8)), hex_color(c.get("color", 0xE0E0E0)),
        )
    elif t == "fill_rect":
        args = "%d, %d, %d, %d, %s" % (
            int(c.get("x", 0)), int(c.get("y", 0)),
            int(c.get("w", 0)), int(c.get("h", 0)),
            hex_color(c.get("color", 0xCCEEFF)),
        )
    elif t == "circle":
        args = "%d, %d, %d, %s" % (
            int(c.get("cx", 0)), int(c.get("cy", 0)), int(c.get("r", 0)),
            hex_color(c.get("color", 0x888888)),
        )
    elif t == "fill_circle":
        args = "%d, %d, %d, %s" % (
            int(c.get("cx", 0)), int(c.get("cy", 0)), int(c.get("r", 0)),
            hex_color(c.get("color", 0x44AA44)),
        )
    elif t == "arc":
        args = "%d, %d, %d, %d, %d, %s" % (
            int(c.get("cx", 0)), int(c.get("cy", 0)), int(c.get("r", 0)),
            int(c.get("start", 0)), int(c.get("sweep", 270)),
            hex_color(c.get("color", 0x666666)),
        )
    elif t == "progress":
        args = "%s, %d, %d, %d, %d, %s" % (
            qstr(c.get("name", "")),
            int(c.get("x", 0)), int(c.get("y", 0)),
            int(c.get("w", 0)), int(c.get("h", 0)),
            qnum(c.get("value", 0)),
        )
    elif t == "textbox":
        args = "%s, %d, %d, %d, %d, %s" % (
            qstr(c.get("name", "")),
            int(c.get("x", 0)), int(c.get("y", 0)),
            int(c.get("w", 0)), int(c.get("h", 0)),
            qstr(c.get("text", "")),
        )
    else:  # 理论不可达
        return "# 未处理的控件类型：%s" % t

    return "%s(%s)" % (func, args)


def export_to_vus(design):
    """把设计数据 dict 转换成 .vus 源码字符串。"""
    name = str(design.get("name", "我的界面"))
    width = int(design.get("width", 480))
    height = int(design.get("height", 320))
    controls = design.get("controls", [])

    lines = []
    lines.append('# 图形界面源码（由 HTML 高级排版设计器导出）')
    lines.append('# 首行参数说明：图形_初始化(逻辑宽度, 逻辑高度, 窗口标题)')
    lines.append('')
    lines.append('图形_初始化(%d, %d, %s)' % (width, height, qstr(name)))

    # 主题（可选）：bg, border, highlight, fg, text
    theme = design.get("theme")
    if isinstance(theme, dict):
        lines.append('图形_主题(%s, %s, %s, %s, %s)' % (
            hex_color(theme.get("bg", 0xFFFFFF)),
            hex_color(theme.get("border", 0x888888)),
            hex_color(theme.get("highlight", 0x0055AA)),
            hex_color(theme.get("fg", 0x333333)),
            hex_color(theme.get("text", 0x000000)),
        ))

    # 全局圆角（可选），radius > 0 才输出
    radius = int(design.get("radius", 0) or 0)
    if radius > 0:
        lines.append('图形_外观(%d)' % radius)

    lines.append('')

    for c in controls:
        name_tag = c.get("name", "")
        if name_tag:
            lines.append('# 控件：%s（%s）' % (name_tag, c.get("type", "")))
        else:
            lines.append('# 控件：%s' % c.get("type", ""))
        lines.append(_emit_control(c))

    lines.append('')
    lines.append('图形_刷新()')
    lines.append('# 共 %d 个控件，界面设计完成。' % len(controls))
    return "\n".join(lines) + "\n"


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) < 1:
        sys.stderr.write("用法: python3 vus_export.py <input.json> [output.vus]\n")
        return 2

    input_path = argv[0]
    output_path = argv[1] if len(argv) > 1 else None

    try:
        with open(input_path, "r", encoding="utf-8") as f:
            design = json.load(f)
    except Exception as err:  # 文件缺失 / JSON 解析错误
        sys.stderr.write("读取输入失败 %s: %s\n" % (input_path, err))
        return 1

    vus_src = export_to_vus(design)

    if output_path:
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(vus_src)
        print("已写出: %s" % output_path)
        print(vus_src)
    else:
        sys.stdout.write(vus_src)
    return 0


if __name__ == "__main__":
    sys.exit(main())