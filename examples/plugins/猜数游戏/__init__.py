"""
猜数游戏 成绩评价插件 — 终端猜数字小游戏配套

输入：回合数（字符串）；输出：星级评价。
由 VUS 侧 插件_运行("猜数游戏", 回合) 调用，验证 .vux 插件链路。
"""

import os
import sys

# 将 scripts 目录加入路径以便导入 vux_plugin_entry
_scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..", "..", "scripts")
if _scripts_dir not in sys.path:
    sys.path.insert(0, _scripts_dir)

from vux_plugin_entry import VuxPlugin


class 猜数游戏插件(VuxPlugin):
    """按回合数给出星级评价（100 以内二分最坏 7 回合）。"""

    def init(self, api):
        self._api = api
        return 0

    def run(self, api, input_data):
        try:
            n = int((input_data or "").strip())
        except ValueError:
            n = 99
        if n <= 4:
            grade = "SSS 神射手"
        elif n <= 6:
            grade = "A 稳健"
        elif n <= 8:
            grade = "B 尚可"
        else:
            grade = "C 继续加油"
        return 0, f"第 {n} 回合猜中 | 评价: {grade}"

    def cleanup(self, api):
        self._api = None
