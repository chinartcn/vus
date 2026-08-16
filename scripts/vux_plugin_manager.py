#!/usr/bin/env python3
"""
vux_plugin_manager.py — VUS .vux 功能插件 / .vulage 语言插件 管理工具

用法:
    python3 vux_plugin_manager.py install <路径或URL>  # 安装 .vux / .vulage 插件
    python3 vux_plugin_manager.py build [目录]         # 打包为 .vux
    python3 vux_plugin_manager.py info <插件名或路径>   # 查看插件信息
    python3 vux_plugin_manager.py list                 # 列出已安装插件
    python3 vux_plugin_manager.py run <插件名> [输入]   # 运行插件
    python3 vux_plugin_manager.py check-deps [目录]    # 检查依赖

.vux 文件结构（功能插件）:
    plugin.vux
    ├── vux.json          # 元数据（必需）
    ├── __init__.py       # 插件入口（必需）
    ├── vux依赖.txt       # 其他 .vux 包依赖（可选）
    ├── vuxpy依赖.txt     # Python 包依赖（可选）
    └── 资源/             # 静态资源（可选）
        ├── 图标.png
        └── 样式.css

.vulage 文件结构（语言插件）:
    plugin.vulage
    ├── vux.json          # 元数据（必需）
    ├── __init__.py       # 预处理入口（必需）
    ├── plugin.vulage     # 共享库（可选，C 实现）
    └── 资源/             # 静态资源（可选）

区别:
    .vux  — 运行时功能插件（TUI、网络、数据库等），在 AST 生成后加载
    .vulage — 编译前语言插件（语法风格），在词法分析前加载
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


# ============================
# 常量
# ============================

VUS_HOME = Path.home() / ".vus"
PLUGINS_DIR = VUS_HOME / "plugins"
VUX_CACHE_DIR = VUS_HOME / "cache"
VUX_JSON_SCHEMA_VERSION = 1

# 插件仓库注册表（可扩展）
REGISTRIES = [
    {
        "name": "官方仓库",
        "url": "https://gitee.com/rtccn_mc/vus-plugins/raw/main/registry.json",
    },
]


# ============================
# vux.json 模式定义
# ============================

VUX_JSON_SCHEMA = {
    "名称": {"type": str, "required": True, "description": "插件名称"},
    "版本": {"type": str, "required": True, "description": "语义化版本号"},
    "作者": {"type": str, "required": False, "description": "作者信息"},
    "描述": {"type": str, "required": False, "description": "插件描述"},
    "入口": {"type": str, "required": False, "default": "__init__.py", "description": "入口文件"},
    "最低VUS版本": {"type": str, "required": False, "description": "兼容的最低 VUS 编译器版本"},
    "依赖": {
        "type": dict,
        "required": False,
        "description": "对其它 .vux 包的依赖",
        "example": {"db.vux": ">=1.2.0", "log.vux": "~=2.0"},
    },
    "Python依赖": {
        "type": dict,
        "required": False,
        "description": "对 Python 包的依赖",
        "example": {"requests": ">=2.28.0", "pillow": "~=9.0"},
    },
}


# ============================
# 核心功能
# ============================

def ensure_dirs():
    """确保插件目录结构存在。"""
    PLUGINS_DIR.mkdir(parents=True, exist_ok=True)
    VUX_CACHE_DIR.mkdir(parents=True, exist_ok=True)


def validate_vux_json(data):
    """验证 vux.json 字段完整性。"""
    errors = []
    for field, schema in VUX_JSON_SCHEMA.items():
        if schema["required"] and field not in data:
            errors.append(f"缺少必需字段: {field} ({schema['description']})")
    if errors:
        return False, errors
    return True, []


def parse_vux_dep(line):
    """解析 vux依赖.txt 中的一行。
    
    支持格式:
        plugin.vux
        plugin.vux >= 1.2.0
        plugin.vux ~= 2.0
        plugin.vux == 1.0.0
    """
    line = line.strip()
    if not line or line.startswith("#"):
        return None

    pattern = r'^([\w\.-]+\.vux)\s*((?:>=|<=|==|~=|!=|>|<)\s*[\w\.]+)?\s*$'
    m = re.match(pattern, line)
    if m:
        name = m.group(1)
        version_spec = m.group(2) or ""
        return {"name": name, "version_spec": version_spec.strip()}
    return None


def parse_py_dep(line):
    """解析 vuxpy依赖.txt 中的一行（pip requirements 格式）。"""
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    return line


def install_python_deps(deps_file):
    """通过 pip 安装 Python 依赖。"""
    if not os.path.isfile(deps_file):
        return True

    print(f"安装 Python 依赖: {deps_file}")
    result = subprocess.run(
        [sys.executable, "-m", "pip", "install", "-r", str(deps_file)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(f"  ⚠ pip 安装警告:\n{result.stderr}")
    return True


def install_vux_deps(deps_file):
    """安装 vux依赖.txt 中列出的其他 .vux 包。"""
    if not os.path.isfile(deps_file):
        return True

    print(f"检查 .vux 依赖: {deps_file}")
    with open(deps_file, "r", encoding="utf-8") as f:
        for line in f:
            dep = parse_vux_dep(line)
            if dep:
                installed_path = PLUGINS_DIR / dep["name"]
                if not installed_path.exists():
                    print(f"  依赖 {dep['name']} 未安装，尝试从仓库获取...")
                    # 尝试从注册表安装
                    install_plugin(dep["name"])


def install_plugin(source):
    """安装插件（.vux 功能插件 或 .vulage 语言插件）。

    Args:
        source: 插件路径、URL 或插件名
    """
    ensure_dirs()
    source = str(source)

    # 判断插件类型
    is_vulage = source.endswith(".vulage") or ".vulage" in source

    if is_vulage:
        # .vulage 语言插件 —— 目录复制，不解压
        source_path = Path(source)
        if not source_path.exists():
            print(f"错误: 找不到 .vulage 插件: {source}", file=sys.stderr)
            return False

        # 读取元数据
        meta_path = source_path / "vux.json"
        if not meta_path.exists():
            print("错误: .vulage 插件中缺少 vux.json", file=sys.stderr)
            return False

        with open(meta_path, "r", encoding="utf-8") as f:
            metadata = json.load(f)

        plugin_name = metadata.get("名称", metadata.get("name", source_path.name))
        install_dir = PLUGINS_DIR / plugin_name

        if install_dir.exists():
            shutil.rmtree(install_dir)

        shutil.copytree(source_path, install_dir)
        print(f"✅ 语言插件 '{plugin_name}' 安装成功 (类型: .vulage)")
        print(f"   安装到: {install_dir}")
        return True

    # .vux 功能插件 —— 原有逻辑（URL 下载/注册表查找/zip 解压）
    if source.startswith(("http://", "https://")):
        # 从 URL 下载
        print(f"下载插件: {source}")
        try:
            import urllib.request
            with tempfile.NamedTemporaryFile(suffix=".vux", delete=False) as tmp:
                urllib.request.urlretrieve(source, tmp.name)
                source = tmp.name
        except Exception as e:
            print(f"下载失败: {e}", file=sys.stderr)
            return False
    elif not source.endswith(".vux") and "/" not in source:
        # 插件名 → 尝试从注册表查找
        # 先在本地查找
        local_path = PLUGINS_DIR / f"{source}.vux"
        if local_path.exists():
            source = str(local_path)
        else:
            print(f"插件 '{source}' 未找到本地文件，尝试从仓库下载...")
            for registry in REGISTRIES:
                url = f"{registry['url'].rstrip('/')}/{source}.vux"
                try:
                    import urllib.request
                    with tempfile.NamedTemporaryFile(suffix=".vux", delete=False) as tmp:
                        urllib.request.urlretrieve(url, tmp.name)
                        source = tmp.name
                    break
                except Exception:
                    continue
            else:
                print(f"错误: 无法找到插件 '{source}'", file=sys.stderr)
                return False

    # 解压 .vux
    print(f"安装: {source}")
    try:
        with zipfile.ZipFile(source, "r") as zf:
            # 验证结构
            if "vux.json" not in zf.namelist():
                print("错误: .vux 文件中缺少 vux.json", file=sys.stderr)
                return False

            # 读取元数据
            metadata = json.loads(zf.read("vux.json").decode("utf-8"))
            valid, errors = validate_vux_json(metadata)
            if not valid:
                for e in errors:
                    print(f"  验证错误: {e}", file=sys.stderr)

            # 确定安装目录
            plugin_name = metadata.get("名称", metadata.get("name", "unknown"))
            install_dir = PLUGINS_DIR / plugin_name

            # 如果已安装，先备份
            if install_dir.exists():
                shutil.rmtree(install_dir)

            # 解压
            zf.extractall(install_dir)
            print(f"  安装到: {install_dir}")

        # 处理依赖
        deps_file = install_dir / "vux依赖.txt"
        install_vux_deps(deps_file)

        py_deps_file = install_dir / "vuxpy依赖.txt"
        install_python_deps(py_deps_file)

        print(f"✅ 功能插件 '{plugin_name}' 安装成功 (类型: .vux)")
        return True

    except zipfile.BadZipFile:
        print("错误: 文件不是有效的 .vux (zip) 格式", file=sys.stderr)
        return False
    except Exception as e:
        print(f"安装失败: {e}", file=sys.stderr)
        return False


def build_plugin(source_dir="."):
    """将插件目录打包为 .vux 文件。
    
    Args:
        source_dir: 插件源码目录（默认当前目录）
    """
    source_dir = Path(source_dir)

    # 验证目录结构
    meta_path = source_dir / "vux.json"
    if not meta_path.exists():
        print("错误: 目录中缺少 vux.json", file=sys.stderr)
        return False

    init_path = source_dir / "__init__.py"
    if not init_path.exists():
        print("错误: 目录中缺少 __init__.py", file=sys.stderr)
        return False

    # 读取元数据
    with open(meta_path, "r", encoding="utf-8") as f:
        metadata = json.load(f)

    valid, errors = validate_vux_json(metadata)
    if not valid:
        for e in errors:
            print(f"  验证错误: {e}", file=sys.stderr)
        return False

    # 确定输出文件名
    plugin_name = metadata.get("名称", metadata.get("name", "unknown"))
    plugin_version = metadata.get("版本", metadata.get("version", "0.0.0"))
    output_name = f"{plugin_name}-{plugin_version}.vux"
    output_path = Path.cwd() / output_name

    # 打包
    print(f"打包插件: {plugin_name} v{plugin_version}")
    with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for file_path in source_dir.rglob("*"):
            if file_path.is_file():
                # 跳过隐藏文件和缓存
                rel_path = file_path.relative_to(source_dir)
                if any(part.startswith(".") or part == "__pycache__" for part in rel_path.parts):
                    continue
                if file_path.suffix in (".pyc", ".pyo"):
                    continue
                zf.write(file_path, rel_path)

    print(f"✅ 输出: {output_path}")
    print(f"   大小: {output_path.stat().st_size / 1024:.1f} KB")
    return True


def show_plugin_info(target):
    """显示插件信息。
    
    Args:
        target: 插件名、.vux 文件路径或插件目录路径
    """
    target = str(target)

    # 判断目标类型
    if target.endswith(".vux"):
        # 从 .vux 文件读取
        try:
            with zipfile.ZipFile(target, "r") as zf:
                if "vux.json" not in zf.namelist():
                    print("错误: 不是有效的 .vux 文件", file=sys.stderr)
                    return
                metadata = json.loads(zf.read("vux.json").decode("utf-8"))
                source = target
        except Exception as e:
            print(f"读取失败: {e}", file=sys.stderr)
            return
    else:
        # 插件名 → 查找已安装的插件
        plugin_dir = PLUGINS_DIR / target
        meta_path = plugin_dir / "vux.json"
        if not meta_path.exists():
            print(f"插件 '{target}' 未安装", file=sys.stderr)
            return
        with open(meta_path, "r", encoding="utf-8") as f:
            metadata = json.load(f)
        source = str(plugin_dir)

    # 显示信息
    print(f"\n  {metadata.get('名称', metadata.get('name', '未命名'))}")
    print(f"  {'=' * 40}")
    print(f"  版本:    {metadata.get('版本', metadata.get('version', '?'))}")
    print(f"  作者:    {metadata.get('作者', metadata.get('author', '?'))}")
    print(f"  描述:    {metadata.get('描述', metadata.get('description', '无'))}")
    print(f"  入口:    {metadata.get('入口', metadata.get('entry', '__init__.py'))}")

    deps = metadata.get("依赖", {})
    if deps:
        print(f"  依赖:    {', '.join(f'{k} {v}' for k, v in deps.items())}")

    py_deps = metadata.get("Python依赖", {})
    if py_deps:
        print(f"  Python:  {', '.join(f'{k} {v}' for k, v in py_deps.items())}")

    print(f"  来源:    {source}")
    print()


def list_plugins():
    """列出所有已安装的插件。"""
    ensure_dirs()

    if not PLUGINS_DIR.exists():
        print("没有已安装的插件。")
        return

    plugins = sorted(PLUGINS_DIR.iterdir())
    if not plugins:
        print("没有已安装的插件。")
        return

    print(f"已安装的插件 ({len(plugins)}):")
    print(f"  {'名称':<24} {'类型':<8} {'版本':<12} {'作者':<16} 描述")
    print(f"  {'-'*24} {'-'*8} {'-'*12} {'-'*16} {'-'*20}")
    for p in plugins:
        if p.is_dir():
            meta_path = p / "vux.json"
            if meta_path.exists():
                try:
                    with open(meta_path, "r", encoding="utf-8") as f:
                        meta = json.load(f)
                    name = meta.get("名称", meta.get("name", p.name))
                    version = meta.get("版本", meta.get("version", "?"))
                    author = meta.get("作者", meta.get("author", ""))
                    desc = meta.get("描述", meta.get("description", ""))

                    # 判断插件类型：是否有 .vulage 文件
                    vulage_files = list(p.glob("*.vulage"))
                    if vulage_files:
                        ptype = ".vulage"
                    else:
                        ptype = ".vux"

                    print(f"  {name:<24} {ptype:<8} {version:<12} {author:<16} {desc}")
                except Exception:
                    pass


def run_plugin(plugin_name, input_data="", raw=False):
    """运行已安装的插件。

    raw=True 时仅输出插件实际返回内容（供 VUS 内建函数 插件_运行 使用），
    不打印生命周期与标签信息。
    """
    from vux_plugin_entry import VuxPluginAPI, load_plugin

    plugin_dir = PLUGINS_DIR / plugin_name
    if not plugin_dir.is_dir():
        print(f"错误: 插件 '{plugin_name}' 未安装", file=sys.stderr)
        return False

    plugin = load_plugin(str(plugin_dir))
    if not plugin:
        print(f"错误: 无法加载插件 '{plugin_name}'", file=sys.stderr)
        return False

    api = VuxPluginAPI()

    # 生命周期
    if not raw:
        print(f"初始化插件: {plugin.name}")
    init_ret = plugin.init(api)
    if init_ret != 0:
        print(f"  初始化失败 (code={init_ret})", file=sys.stderr)
        return False

    if not raw:
        print(f"运行插件: {plugin.name}")
    ret, output = plugin.run(api, input_data)
    if raw:
        # raw 模式：只输出插件返回值（stdout 上的 output），返回码走退出状态
        plugin.cleanup(api)
        if output:
            print(output)
        sys.exit(0 if ret == 0 else 1)

    print(f"  返回码: {ret}")
    if output:
        print(f"  输出: {output}")

    plugin.cleanup(api)
    print(f"清理完成")
    return True


def check_dependencies(source_dir="."):
    """检查插件依赖是否满足。"""
    source_dir = Path(source_dir)

    meta_path = source_dir / "vux.json"
    if not meta_path.exists():
        print("错误: 目录中缺少 vux.json", file=sys.stderr)
        return False

    with open(meta_path, "r", encoding="utf-8") as f:
        metadata = json.load(f)

    all_ok = True

    # 检查 .vux 依赖
    deps = metadata.get("依赖", {})
    if deps:
        print("检查 .vux 依赖:")
        for dep_name, spec in deps.items():
            installed_dir = PLUGINS_DIR / dep_name
            if installed_dir.is_dir():
                print(f"  ✅ {dep_name} {spec} — 已安装")
            else:
                print(f"  ❌ {dep_name} {spec} — 未安装")
                all_ok = False

    # 检查 vux依赖.txt
    deps_file = source_dir / "vux依赖.txt"
    if deps_file.exists():
        with open(deps_file, "r", encoding="utf-8") as f:
            for line in f:
                dep = parse_vux_dep(line)
                if dep:
                    installed_dir = PLUGINS_DIR / dep["name"]
                    if installed_dir.is_dir():
                        print(f"  ✅ {dep['name']} — 已安装")
                    else:
                        print(f"  ❌ {dep['name']} — 未安装")
                        all_ok = False

    # 检查 Python 依赖
    py_deps = metadata.get("Python依赖", {})
    if py_deps:
        print("检查 Python 依赖:")
        for pkg, spec in py_deps.items():
            try:
                __import__(pkg)
                print(f"  ✅ {pkg} {spec} — 已安装")
            except ImportError:
                print(f"  ❌ {pkg} {spec} — 未安装")
                all_ok = False

    py_deps_file = source_dir / "vuxpy依赖.txt"
    if py_deps_file.exists():
        with open(py_deps_file, "r", encoding="utf-8") as f:
            for line in f:
                dep = parse_py_dep(line)
                if dep:
                    pkg_name = dep.split()[0].replace(">=", "").replace("==", "").strip()
                    try:
                        __import__(pkg_name)
                        print(f"  ✅ {dep} — 已安装")
                    except ImportError:
                        print(f"  ❌ {dep} — 未安装")
                        all_ok = False

    if all_ok:
        print("所有依赖满足 ✅")
    else:
        print("存在未满足的依赖 ❌ 请运行 install 命令安装")
    return all_ok


# ============================
# CLI 入口
# ============================

def main():
    parser = argparse.ArgumentParser(
        description="VUS 插件管理工具 (.vux)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python3 vux_plugin_manager.py build ./my-plugin
  python3 vux_plugin_manager.py install my-plugin.vux
  python3 vux_plugin_manager.py info 示例
  python3 vux_plugin_manager.py list
  python3 vux_plugin_manager.py run 示例 "Hello"
  python3 vux_plugin_manager.py check-deps
        """,
    )

    subparsers = parser.add_subparsers(dest="command", help="子命令")

    # install
    p_install = subparsers.add_parser("install", help="安装 .vux 插件")
    p_install.add_argument("source", help=".vux 文件路径、URL 或插件名")

    # build
    p_build = subparsers.add_parser("build", help="打包为 .vux")
    p_build.add_argument("source_dir", nargs="?", default=".", help="插件源码目录")

    # info
    p_info = subparsers.add_parser("info", help="查看插件信息")
    p_info.add_argument("target", help="插件名或 .vux 文件路径")

    # list
    subparsers.add_parser("list", help="列出已安装插件")

    # run
    p_run = subparsers.add_parser("run", help="运行插件")
    p_run.add_argument("plugin_name", help="插件名")
    p_run.add_argument("input", nargs="?", default="", help="输入数据")
    p_run.add_argument("--raw", action="store_true",
                       help="仅输出插件实际返回内容（供 VUS 内建函数调用）")

    # check-deps
    p_check = subparsers.add_parser("check-deps", help="检查依赖")
    p_check.add_argument("source_dir", nargs="?", default=".", help="插件目录")

    args = parser.parse_args()

    if args.command == "install":
        install_plugin(args.source)
    elif args.command == "build":
        build_plugin(args.source_dir)
    elif args.command == "info":
        show_plugin_info(args.target)
    elif args.command == "list":
        list_plugins()
    elif args.command == "run":
        run_plugin(args.plugin_name, args.input, raw=args.raw)
    elif args.command == "check-deps":
        check_dependencies(args.source_dir)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()