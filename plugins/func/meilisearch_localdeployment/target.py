"""平台 / 架构检测与 Meilisearch 官方二进制选择。

模块名为 target 而非 platform，避免与 Python 标准库 platform 冲突。

Meilisearch 官方静态二进制按 ``<os>-<arch>`` 命名，各平台可用情况：

- Linux x86_64  -> meilisearch-linux-amd64
- Linux arm64   -> meilisearch-linux-aarch64
- Linux arm32   -> 仅提供 musl(Alpine) 的 armv7 二进制；glibc 无 arm32 二进制
- macOS         -> meilisearch-darwin-amd64 / meilisearch-darwin-aarch64
- Windows       -> meilisearch-windows-amd64.exe
"""

import json
import os
import urllib.request

# 非 Alpine 的 arm32 Linux 无官方二进制，标记为"需从源码构建"
BUILD_FROM_SOURCE = "build-from-source"

# GitHub Releases 相关地址
GITHUB_API_LATEST = "https://api.github.com/repos/meilisearch/meilisearch/releases/latest"
GITHUB_DOWNLOAD_BASE = "https://github.com/meilisearch/meilisearch/releases/download"


def detect_os():
    """返回当前操作系统标识：linux / darwin / windows / 其它。"""
    sysname = os.uname().sysname.lower() if hasattr(os, "uname") else ""
    if sysname.startswith("linux"):
        return "linux"
    if sysname.startswith("darwin"):
        return "darwin"
    if os.name == "nt" or sysname.startswith("windows"):
        return "windows"
    return sysname or "unknown"


def detect_arch():
    """返回 CPU 架构标识：x86_64 / arm64 / arm32 / x86 / 其它。"""
    machine = os.uname().machine.lower() if hasattr(os, "uname") else ""
    if machine in ("x86_64", "amd64"):
        return "x86_64"
    if machine in ("aarch64", "arm64"):
        return "arm64"
    if machine.startswith("arm"):
        # armv7l / armv6l / armv8l(32位用户态) 均视为 arm32
        return "arm32"
    if machine in ("i386", "i686", "x86"):
        return "x86"
    return machine or "unknown"


def is_alpine():
    """检测是否为 Alpine Linux（musl libc）。依赖 /etc/alpine-release 存在。"""
    return os.path.isfile("/etc/alpine-release")


def resolve_asset(os_name=None, arch=None):
    """根据操作系统与架构解析官方二进制资产名。

    Args:
        os_name: 操作系统标识，缺省用 detect_os()
        arch: 架构标识，缺省用 detect_arch()

    Returns:
        资产文件名（如 "meilisearch-linux-amd64"）；非 Alpine 的 arm32
        Linux 返回常量 BUILD_FROM_SOURCE。
    """
    os_name = os_name or detect_os()
    arch = arch or detect_arch()

    if os_name == "linux":
        if arch == "x86_64":
            return "meilisearch-linux-amd64"
        if arch == "arm64":
            return "meilisearch-linux-aarch64"
        if arch == "arm32":
            if is_alpine():
                return "meilisearch-linux-armv7"
            return BUILD_FROM_SOURCE
        if arch == "x86":
            return "meilisearch-linux-i686"
        return None
    if os_name == "darwin":
        if arch in ("x86_64", "arm64"):
            return f"meilisearch-darwin-{arch}"
        return None
    if os_name == "windows":
        if arch in ("x86_64", "x86"):
            suffix = "amd64" if arch == "x86_64" else "i686"
            return f"meilisearch-windows-{suffix}.exe"
        return None
    return None


def get_latest_version():
    """查询 Meilisearch 最新发布版本号（去掉前缀 v）。

    Returns:
        版本号字符串（如 "1.8.0"）；网络失败或解析失败返回 None。
    """
    try:
        with urllib.request.urlopen(GITHUB_API_LATEST, timeout=15) as resp:
            data = resp.read().decode("utf-8")
        tag = json.loads(data).get("tag_name", "")
        return tag.lstrip("v") or None
    except Exception:
        return None


def download_url(version, asset):
    """构造指定版本与资产的下载地址。"""
    version = version.lstrip("v")
    return f"{GITHUB_DOWNLOAD_BASE}/v{version}/{asset}"