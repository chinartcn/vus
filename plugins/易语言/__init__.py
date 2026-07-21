"""
易语言语法插件 — 将易语言风格语法转换为标准 VUS 函数风格语法

转换规则（关键字）：
  .功能 name(params)   → 定义 name(params):
  .如果 cond           → 如果 cond:
  .否则如果 cond       → 否则如果 cond:
  .否则               → 否则:
  .计次循环 var, s, e  → 循环 var 从 s 到 e:
  .遍历 var, iterable  → 循环 var 在 iterable:
  .循环               → 当循环 True:
  .返回 expr          → 返回 expr
  .抛出 expr          → 抛出 expr
  .导入 mod           → 导入 mod
  .从 mod 导入 name   → 从 mod 导入 name
  .跳出               → 跳出
  .全局 var           → 全局 var
  .尝试               → 尝试:
  .捕获               → 捕获:

其他规则：
  .结束               → 删除该行
  .标识符              → 标识符（去掉点前缀）
"""

import os
import sys
import re

# 将 scripts 目录加入路径以便导入 vux_plugin_entry
_scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..", "scripts")
if _scripts_dir not in sys.path:
    sys.path.insert(0, _scripts_dir)

from vux_plugin_entry import VuxPlugin, VuxPluginAPI


class 易语言(VuxPlugin):
    """易语言语法插件 — 将易语言风格语法转换为标准 VUS 函数风格语法。"""

    # 易语言关键字 → 函数风格关键字的映射
    KEYWORD_MAP = {
        "功能":     "定义",
        "如果":     "如果",
        "否则如果": "否则如果",
        "否则":     "否则",
        "计次循环": "计次循环",  # 特殊处理
        "遍历":     "遍历",      # 特殊处理
        "循环":     "循环",      # 特殊处理
        "返回":     "返回",
        "抛出":     "抛出",
        "导入":     "导入",
        "从":       "从",
        "跳出":     "跳出",
        "全局":     "全局",
        "尝试":     "尝试",
        "捕获":     "捕获",
        "结束":     None,  # 删除该行
    }

    def init(self, api):
        """初始化插件。"""
        self._api = api
        return 0

    def run(self, api, input_data):
        """执行插件：将输入的易语言风格代码转换为函数风格代码。

        Args:
            api: VuxPluginAPI 实例
            input_data: 易语言风格 VUS 源码

        Returns:
            (code, output): 返回码和转换后的代码
        """
        if not input_data:
            return 0, ""

        output = self._convert(input_data)
        return 0, output

    def _convert(self, source):
        """将易语言风格源码转换为函数风格源码。"""
        lines = source.split("\n")
        result = []
        i = 0
        while i < len(lines):
            line = lines[i]
            stripped = line.lstrip()
            indent = line[:len(line) - len(stripped)]

            # 空行或纯注释行
            if not stripped or stripped.startswith("#") or stripped.startswith("//"):
                result.append(line)
                i += 1
                continue

            # 检查是否以 . 开头（易语言风格）
            if stripped.startswith("."):
                # 处理 .结束
                if stripped == ".结束" or stripped.startswith(".结束 ") or stripped.startswith(".结束\t"):
                    # 跳过该行，不输出
                    i += 1
                    continue

                # 处理 .关键字 模式
                converted = self._convert_line(stripped, indent)
                result.append(converted)
                i += 1
            else:
                # 非易语言风格行，保持不变
                result.append(line)
                i += 1

        return "\n".join(result)

    def _convert_line(self, stripped, indent):
        """转换单行易语言代码。"""
        # 去掉开头的 .
        content = stripped[1:]

        # 尝试匹配关键字
        # 先尝试最长匹配
        for kw_len in sorted([len(k) for k in self.KEYWORD_MAP.keys()], reverse=True):
            for kw, target in self.KEYWORD_MAP.items():
                if len(kw) != kw_len:
                    continue
                if content.startswith(kw):
                    # 关键字后面应该有空白、行尾或 (
                    rest = content[len(kw):]
                    if rest == "" or rest[0] in (" ", "\t", "(", "，", ","):
                        if target is None:
                            return indent + rest.lstrip()  # 删除关键字
                        return self._build_line(indent, target, rest, kw)

        # 不是关键字，普通 .标识符 → 去掉点
        return indent + content

    def _build_line(self, indent, target, rest, kw):
        """根据关键字构建转换后的行。"""
        rest = rest.strip()

        # 特殊处理：计次循环 var, start, end → 循环 var 从 start 到 end:
        if kw == "计次循环":
            parts = self._split_comma(rest)
            if len(parts) >= 3:
                var = parts[0].strip()
                start = parts[1].strip()
                end = parts[2].strip()
                return f"{indent}循环 {var} 从 {start} 到 {end}:"
            else:
                return f"{indent}循环 {rest}:"  # 保底

        # 特殊处理：遍历 var, iterable → 循环 var 在 iterable:
        if kw == "遍历":
            parts = self._split_comma(rest)
            if len(parts) >= 2:
                var = parts[0].strip()
                iterable = parts[1].strip()
                return f"{indent}循环 {var} 在 {iterable}:"
            else:
                return f"{indent}循环 {rest}:"  # 保底

        # 特殊处理：.循环（无限循环）→ 当循环 True:
        if kw == "循环" and not rest:
            return f"{indent}当循环 True:"

        # 特殊处理：需要冒号的关键字
        need_colon = {"如果", "否则如果", "否则", "尝试", "捕获"}
        if kw in need_colon:
            if rest:
                return f"{indent}{target} {rest}:"
            else:
                return f"{indent}{target}:"

        # 特殊处理：.功能 name(params) → 定义 name(params):
        if kw == "功能":
            return f"{indent}定义 {rest}:"

        # 特殊处理：.从 mod 导入 name → 从 mod 导入 name
        if kw == "从":
            # rest 是模块名，后面应该跟 .导入
            # 但我们的转换在行级别，所以直接保留
            return f"{indent}从 {rest}"

        # 普通关键字：直接替换
        if rest:
            return f"{indent}{target} {rest}"
        else:
            return f"{indent}{target}"

    def _split_comma(self, text):
        """按逗号分割（支持中文逗号）。"""
        parts = []
        depth = 0
        current = ""
        for ch in text:
            if ch in ("(", "（", "["):
                depth += 1
                current += ch
            elif ch in (")", "）", "]"):
                depth -= 1
                current += ch
            elif ch in (",", "，") and depth == 0:
                parts.append(current)
                current = ""
            else:
                current += ch
        if current:
            parts.append(current)
        return parts

    def cleanup(self, api):
        """清理插件资源。"""
        self._api = None