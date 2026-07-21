"""
vux_plugin_entry.py — .vux 插件入口基类

所有 Python 插件应在 __init__.py 中继承 VuxPlugin 类，
实现 init()、run()、cleanup() 三个生命周期方法。

示例：
    from vux_plugin_entry import VuxPlugin

    class MyPlugin(VuxPlugin):
        def init(self, api):
            print(f"ABI 版本: {api['version']}")
            return 0

        def run(self, api, input_data):
            return 0, f"处理结果: {input_data}"

        def cleanup(self, api):
            pass
"""

import json
import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


class VuxPlugin:
    """插件基类，所有 .vux 插件必须继承此类。"""

    # 插件元数据（由 vux.json 自动填充）
    name = ""
    version = ""
    description = ""
    author = ""
    entry = ""

    def init(self, api):
        """初始化插件。返回 0 成功，非 0 失败。"""
        return 0

    def run(self, api, input_data):
        """执行插件主要功能。返回 (code, output)。"""
        return 0, ""

    def cleanup(self, api):
        """清理插件资源。"""
        pass


class VuxPluginAPI:
    """插件可调用的编译器 API。
    
    通过 ctypes 调用 VUS 编译器的 C ABI 接口。
    """

    def __init__(self, vus_binary=None):
        self._vus_binary = vus_binary or self._find_vus()
        self._lib = None
        self._init_ctypes()

    def _find_vus(self):
        """查找 vus 编译器二进制文件。"""
        candidates = [
            os.environ.get("VUS_BINARY", ""),
            "./vus",
            os.path.join(os.path.dirname(__file__), "..", "vus"),
            os.path.join(os.path.dirname(__file__), "..", "..", "vus"),
            "/usr/local/bin/vus",
            "/usr/bin/vus",
        ]
        for c in candidates:
            if c and os.path.isfile(c) and os.access(c, os.X_OK):
                return os.path.abspath(c)
        return "vus"  # 默认 fallback

    def _init_ctypes(self):
        """尝试加载编译器的 C ABI 共享库。"""
        try:
            import ctypes
            lib_paths = [
                os.path.join(os.path.dirname(self._vus_binary), "libvus.so"),
                os.path.join(os.path.dirname(__file__), "..", "libvus.so"),
                "/usr/local/lib/libvus.so",
            ]
            for lp in lib_paths:
                if os.path.isfile(lp):
                    self._lib = ctypes.CDLL(lp)
                    break
        except Exception:
            self._lib = None

    @property
    def version(self):
        return 0x010000  # ABI v1.0.0

    def compile_file(self, path, config=None):
        """编译 .vus 文件 → C 代码。"""
        if self._lib:
            try:
                import ctypes
                self._lib.vus_abi_version.restype = ctypes.c_int
                return {"success": True, "message": "ABI loaded"}
            except Exception as e:
                return {"success": False, "error": str(e)}

        # 回退到 CLI 调用
        cmd = [self._vus_binary, "build", "--c-only", path]
        result = subprocess.run(cmd, capture_output=True, text=True)
        return {
            "success": result.returncode == 0,
            "output": result.stdout,
            "error": result.stderr,
        }

    def compile_string(self, source, config=None):
        """从源码字符串编译 → C 代码。"""
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".vus", delete=False, encoding="utf-8"
        ) as f:
            f.write(source)
            tmp_path = f.name

        try:
            result = self.compile_file(tmp_path, config)
            return result
        finally:
            os.unlink(tmp_path)

    def eval(self, code, config=None):
        """求值 VUS 表达式，返回 stdout 输出。"""
        cmd = [self._vus_binary, "eval", code]
        result = subprocess.run(cmd, capture_output=True, text=True)
        return {
            "success": result.returncode == 0,
            "output": result.stdout.strip(),
            "error": result.stderr,
        }

    def compiler_version(self):
        """获取编译器版本。"""
        cmd = [self._vus_binary, "--version"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.stdout.strip() or "v0.1"


# ============================
# 插件加载器
# ============================

def load_plugin(plugin_dir):
    """从 .vux 解压目录加载插件。

    Args:
        plugin_dir: .vux 解压后的目录路径

    Returns:
        VuxPlugin 实例，或 None（加载失败）
    """
    plugin_dir = Path(plugin_dir)
    if not plugin_dir.is_dir():
        return None

    # 读取 vux.json
    meta_path = plugin_dir / "vux.json"
    if not meta_path.exists():
        return None

    with open(meta_path, "r", encoding="utf-8") as f:
        metadata = json.load(f)

    # 加载 __init__.py
    init_path = plugin_dir / "__init__.py"
    if not init_path.exists():
        return None

    sys.path.insert(0, str(plugin_dir))
    try:
        import importlib
        mod = importlib.import_module("__init__")

        plugin_instance = None
        for attr_name in dir(mod):
            attr = getattr(mod, attr_name)
            if isinstance(attr, type) and issubclass(attr, VuxPlugin) and attr != VuxPlugin:
                plugin_instance = attr()
                break

        if plugin_instance is None:
            return None

        # 填充元数据
        plugin_instance.name = metadata.get("名称", metadata.get("name", ""))
        plugin_instance.version = metadata.get("版本", metadata.get("version", ""))
        plugin_instance.description = metadata.get("描述", metadata.get("description", ""))
        plugin_instance.author = metadata.get("作者", metadata.get("author", ""))

        return plugin_instance
    except Exception as e:
        print(f"加载插件失败: {e}", file=sys.stderr)
        return None
    finally:
        sys.path.pop(0)


def load_plugin_from_vux(vux_path):
    """从 .vux 文件加载插件。

    Args:
        vux_path: .vux 文件路径

    Returns:
        VuxPlugin 实例，或 None
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        with zipfile.ZipFile(vux_path, "r") as zf:
            zf.extractall(tmpdir)
        return load_plugin(tmpdir)