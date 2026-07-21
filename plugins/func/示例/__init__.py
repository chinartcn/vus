"""
示例插件 — VUS 插件演示

展示插件生命周期（init → run → cleanup）和 VUS 编译器调用。
"""

import os
import sys

# 将 scripts 目录加入路径以便导入 vux_plugin_entry
_scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..", "..", "scripts")
if _scripts_dir not in sys.path:
    sys.path.insert(0, _scripts_dir)

from vux_plugin_entry import VuxPlugin, VuxPluginAPI


class 示例插件(VuxPlugin):
    """示例插件 — 演示插件系统的完整生命周期。"""

    def init(self, api):
        """初始化插件。
        
        在插件加载后、使用前调用。
        返回 0 成功，非 0 失败。
        """
        print(f"  [示例插件] init() — 编译器版本: {api.compiler_version()}")
        self._api = api
        return 0

    def run(self, api, input_data):
        """执行插件主要功能。
        
        Args:
            api: VuxPluginAPI 实例
            input_data: 输入数据字符串

        Returns:
            (code, output): 返回码和输出字符串
        """
        print(f"  [示例插件] run() — 输入: {input_data or '(空)'}")

        # 演示 1: 编译 VUS 代码
        vus_code = '打印("Hello from VUS plugin!\\n")'
        result = api.compile_string(vus_code)
        if result.get("success"):
            output = "VUS 编译成功"
        else:
            output = f"VUS 编译失败: {result.get('error', '未知错误')}"

        # 演示 2: 调用 VUS 编译器 CLI
        import subprocess
        vus_bin = os.environ.get("VUS_BINARY", "./vus")
        try:
            cli_result = subprocess.run(
                [vus_bin, "--version"],
                capture_output=True, text=True, timeout=5
            )
            version = cli_result.stdout.strip() or "v0.1"
            output += f" | 编译器: {version}"
        except Exception:
            output += " | 编译器: 未找到"

        return 0, output

    def cleanup(self, api):
        """清理插件资源。"""
        print(f"  [示例插件] cleanup()")
        self._api = None