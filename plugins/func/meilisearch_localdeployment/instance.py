"""命名实例管理：配置、数据目录、PID/日志文件。

实例默认目录：
- 配置: ~/.vus/plugins/meilisearch_localdeployment/<name>.json
- 数据: ~/.vus/data/meilisearch/<name>/
- PID/日志: ~/.vus/plugins/meilisearch_localdeployment/<name>.pid / <name>.log
"""

import json
import os
import secrets
from pathlib import Path

PLUGIN_DIR = Path("~/.vus/plugins/meilisearch_localdeployment").expanduser()
DATA_ROOT = Path("~/.vus/data/meilisearch").expanduser()

DEFAULT_PORT = 7700


class InstanceManager:
    """管理多个 Meilisearch 命名实例。"""

    def __init__(self, plugin_dir=None, data_root=None):
        self.plugin_dir = Path(plugin_dir) if plugin_dir else PLUGIN_DIR
        self.data_root = Path(data_root) if data_root else DATA_ROOT

    def instance_path(self, name):
        return self.plugin_dir / f"{name}.json"

    def pid_path(self, name):
        return self.plugin_dir / f"{name}.pid"

    def log_path(self, name):
        return self.plugin_dir / f"{name}.log"

    def data_dir(self, name):
        return self.data_root / name

    def list_instances(self):
        """列出所有已保存的实例名。"""
        if not self.plugin_dir.is_dir():
            return []
        names = []
        for p in sorted(self.plugin_dir.glob("*.json")):
            names.append(p.stem)
        return names

    def load_instance(self, name):
        """读取实例配置；不存在返回 None。"""
        path = self.instance_path(name)
        if not path.is_file():
            return None
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)

    def save_instance(self, name, cfg):
        """保存实例配置（自动生成随机 master key 若未提供）。"""
        cfg.setdefault("name", name)
        cfg.setdefault("port", DEFAULT_PORT)
        cfg.setdefault("master_key", secrets.token_hex(16))
        cfg.setdefault("data_dir", str(self.data_dir(name)))
        self.plugin_dir.mkdir(parents=True, exist_ok=True)
        with open(self.instance_path(name), "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)

    def delete_instance(self, name):
        """删除实例配置与 PID/日志文件（不删除数据目录）。"""
        removed = []
        for p in (self.instance_path(name), self.pid_path(name), self.log_path(name)):
            if p.is_file():
                p.unlink()
                removed.append(str(p))
        return removed