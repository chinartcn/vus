"""Meilisearch 本地部署拓展插件入口。

中文子命令路由：
- 部署 信息 / 安装 / 启动 / 停止 / 重启 / 状态 / 更新 / 卸载 / 日志 / 配置 / 集成
- 实例 列表 / 创建 / 删除
"""

import os
import sys
from pathlib import Path

# 确保本插件目录在 sys.path，便于子模块绝对导入
_PLUGIN_DIR = Path(__file__).parent
if str(_PLUGIN_DIR) not in sys.path:
    sys.path.insert(0, str(_PLUGIN_DIR))

# 将 scripts 目录加入路径以便导入 vux_plugin_entry（独立运行/测试时）
_scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..", "..", "scripts")
if _scripts_dir not in sys.path:
    sys.path.insert(0, _scripts_dir)

try:
    from vux_plugin_entry import VuxPlugin
except ImportError:
    class VuxPlugin:
        """最小回退基类：scripts 目录不可用时保证插件仍可独立运行。"""
        name = ""
        version = ""
        description = ""
        author = ""

        def init(self, api):
            return 0

        def run(self, api, input_data):
            return 0, ""

        def cleanup(self, api):
            pass

import deployer
import instance as inst
import integration
import target


class MeilisearchLocalDeploymentPlugin(VuxPlugin):
    """Meilisearch 本地部署拓展 .vux 功能插件。"""

    name = "Meilisearch本地部署拓展"
    version = "1.0.0"
    description = "在本地部署并管理 Meilisearch 搜索服务，深度集成到 meilisearch 插件"

    def __init__(self, mgr=None):
        self.mgr = mgr or inst.InstanceManager()

    # ------------------------------------------------------------------
    # VUS 插件接口
    # ------------------------------------------------------------------
    def init(self, api):
        return 0

    def cleanup(self, api):
        return 0

    def run(self, api, input_data):
        rest, options = self._parse(input_data)
        return self._route(rest, options)

    # ------------------------------------------------------------------
    # 命令路由
    # ------------------------------------------------------------------
    def _parse(self, input_data):
        """将输入拆分为 (rest_tokens, options_dict)。"""
        parts = (input_data or "").split()
        rest = []
        options = {}
        i = 0
        while i < len(parts):
            tok = parts[i]
            if tok.startswith("--"):
                key = tok[2:]
                if i + 1 < len(parts) and not parts[i + 1].startswith("--"):
                    options[key] = parts[i + 1].strip("\"'")
                    i += 2
                else:
                    options[key] = True
                    i += 1
            else:
                rest.append(tok)
                i += 1
        return rest, options

    def _route(self, rest, options):
        if not rest:
            return 0, "用法: 部署 <子命令> 或 实例 <子命令>。示例: 部署 信息 / 部署 启动 / 实例 列表"

        root = rest[0]
        sub = rest[1] if len(rest) > 1 else None

        if root == "部署":
            return self._cmd_deploy(sub, rest[2:], options)
        if root == "实例":
            return self._cmd_instance(sub, rest[2:], options)
        return 0, f"错误: 未知命令 '{root}'，仅支持 部署 / 实例"

    # ------------------------------------------------------------------
    # 部署子命令
    # ------------------------------------------------------------------
    def _cmd_deploy(self, sub, args, options):
        if sub == "信息":
            return self._deploy_info()
        if sub == "安装":
            return self._deploy_install(options)
        if sub == "启动":
            return self._deploy_start(options)
        if sub == "停止":
            return self._deploy_stop(options)
        if sub == "重启":
            return self._deploy_restart(options)
        if sub == "状态":
            return self._deploy_status(options)
        if sub == "更新":
            return self._deploy_update(options)
        if sub == "卸载":
            return self._deploy_uninstall(options)
        if sub == "日志":
            return self._deploy_log(options)
        if sub == "配置":
            return self._deploy_config(options)
        if sub == "集成":
            return self._deploy_integrate(options)
        return 0, "错误: 未知部署子命令（信息/安装/启动/停止/重启/状态/更新/卸载/日志/配置/集成）"

    # ------------------------------------------------------------------
    # 实例子命令
    # ------------------------------------------------------------------
    def _cmd_instance(self, sub, args, options):
        if sub == "列表":
            names = self.mgr.list_instances()
            if not names:
                return 0, "暂无实例。可执行 部署 启动 创建默认实例。"
            return 0, "实例:\n" + "\n".join(f"  - {n}" for n in names)
        if sub == "创建":
            name = options.get("实例") or (args[0] if args else "default")
            self.mgr.save_instance(name, {
                "port": int(options.get("端口", inst.DEFAULT_PORT)),
                "master_key": options.get("master-key"),
                "version": options.get("版本") or "latest",
            })
            return 0, f"实例 '{name}' 已创建"
        if sub == "删除":
            name = options.get("实例") or (args[0] if args else None)
            if not name:
                return 0, "错误: 实例 删除 <名称>"
            removed = self.mgr.delete_instance(name)
            return 0, f"实例 '{name}' 已删除" + (f"（清理: {'，'.join(removed)}）" if removed else "（未清理文件）")
        return 0, "错误: 未知实例子命令（列表/创建/删除）"

    # ------------------------------------------------------------------
    # 部署实现
    # ------------------------------------------------------------------
    def _deploy_info(self):
        d = deployer.detect_deployer()
        if d == target.BUILD_FROM_SOURCE:
            mode = "不支持（arm32 非 Alpine Linux，需源码构建）"
        else:
            mode = d.mode
        latest = target.get_latest_version()
        lat = f"；最新 {latest}" if latest else ""
        instances = self.mgr.list_instances()
        lines = [
            f"部署方式: {mode}",
            f"可用版本{lat}",
            f"实例: {', '.join(instances) if instances else '无'}",
        ]
        return 0, "\n".join(lines)

    def _resolve_deployer(self):
        d = deployer.detect_deployer()
        if d == target.BUILD_FROM_SOURCE:
            return None, "当前平台（arm32 非 Alpine Linux）无官方二进制且无 Docker，请从源码构建 Meilisearch。"
        return d, None

    def _get_instance(self, options, args):
        name = options.get("实例") or (args[0] if args else "default")
        cfg = self.mgr.load_instance(name)
        if cfg is None:
            cfg = {
                "name": name,
                "port": int(options.get("端口", inst.DEFAULT_PORT)),
                "master_key": options.get("master-key"),
                "version": options.get("版本") or "latest",
                "data_dir": str(self.mgr.data_dir(name)),
            }
            self.mgr.save_instance(name, cfg)
        else:
            if options.get("端口"):
                cfg["port"] = int(options["端口"])
            if options.get("master-key"):
                cfg["master_key"] = options["master-key"]
            if options.get("数据目录"):
                cfg["data_dir"] = options["数据目录"]
            if options.get("版本"):
                cfg["version"] = options["版本"]
            self.mgr.save_instance(name, cfg)
        # 二进制部署需要日志路径
        cfg["_log_path"] = str(self.mgr.log_path(name))
        return cfg

    def _deploy_install(self, options):
        d, err = self._resolve_deployer()
        if err:
            return 0, err
        version = options.get("版本") or target.get_latest_version() or "latest"
        cfg = {
            "name": options.get("实例") or "default",
            "version": version,
            "master_key": options.get("master-key"),
            "port": int(options.get("端口", inst.DEFAULT_PORT)),
        }
        if d.mode == "docker":
            r = d.install(version, cfg)
            ok = r.returncode == 0
            return 0, (f"Docker 镜像拉取成功: v{version}" if ok else f"Docker 拉取失败: {r.stderr.strip()}")
        dest, err = d.install(version, cfg)
        if err:
            return 0, err
        return 0, f"二进制下载成功: {dest}"

    def _deploy_start(self, options):
        d, err = self._resolve_deployer()
        if err:
            return 0, err
        cfg = self._get_instance(options, [])
        if d.mode == "docker":
            r = d.start(cfg)
            ok = r.returncode == 0
            return 0, (f"容器已启动: vus-meilisearch-{cfg['name']} (端口 {cfg['port']})"
                       if ok else f"启动失败: {r.stderr.strip()}")
        proc, err = d.start(cfg)
        if err:
            return 0, err
        if proc:
            cfg["_pid"] = proc.pid
            self.mgr.save_instance(cfg["name"], cfg)
            return 0, f"已启动 (PID {proc.pid}, 端口 {cfg['port']})"
        return 0, "启动失败"

    def _deploy_stop(self, options):
        d, err = self._resolve_deployer()
        if err:
            return 0, err
        cfg = self._get_instance(options, [])
        if d.mode == "docker":
            r = d.stop(cfg)
            return 0, ("已停止" if r.returncode == 0 else f"停止失败: {r.stderr.strip()}")
        ok, err = d.stop(cfg)
        return 0, "已停止" if (ok and not err) else f"停止: {err or '失败'}"

    def _deploy_restart(self, options):
        d, err = self._resolve_deployer()
        if err:
            return 0, err
        cfg = self._get_instance(options, [])
        if d.mode == "docker":
            r = d.restart(cfg)
            return 0, ("已重启" if r.returncode == 0 else f"重启失败: {r.stderr.strip()}")
        proc, err = d.restart(cfg)
        if proc:
            cfg["_pid"] = proc.pid
            self.mgr.save_instance(cfg["name"], cfg)
            return 0, f"已重启 (PID {proc.pid})"
        return 0, f"重启失败: {err or '未知'}"

    def _deploy_status(self, options):
        d, err = self._resolve_deployer()
        if err:
            return 0, err
        cfg = self._get_instance(options, [])
        if d.mode == "docker":
            s = d.status(cfg)
            return 0, s or f"容器 vus-meilisearch-{cfg['name']} 不存在或未运行"
        return 0, d.status(cfg)

    def _deploy_update(self, options):
        version = options.get("版本")
        if not version:
            return 0, "错误: 部署 更新 需要 --版本 vX.Y.Z"
        d, err = self._resolve_deployer()
        if err:
            return 0, err
        cfg = self._get_instance(options, [])
        if d.mode == "docker":
            r = d.update(version, cfg)
            return 0, (f"已更新到 v{version}" if r.returncode == 0 else f"更新失败: {r.stderr.strip()}")
        res, err = d.update(version, cfg)
        if err:
            return 0, err
        return 0, f"已更新到 v{version}: {res}"

    def _deploy_uninstall(self, options):
        d, err = self._resolve_deployer()
        if err:
            return 0, err
        cfg = self._get_instance(options, [])
        if d.mode == "docker":
            r = d.uninstall(cfg)
            return 0, ("已卸载容器" if r.returncode == 0 else f"卸载失败: {r.stderr.strip()}")
        return 0, d.uninstall(cfg)

    def _deploy_log(self, options):
        d, err = self._resolve_deployer()
        if err:
            return 0, err
        cfg = self._get_instance(options, [])
        return 0, d.run_log(cfg)

    def _deploy_config(self, options):
        cfg = self._get_instance(options, [])
        lines = [
            f"实例:    {cfg['name']}",
            f"端口:    {cfg['port']}",
            f"版本:    {cfg.get('version', 'latest')}",
            f"数据目录: {cfg.get('data_dir')}",
            f"master key: {cfg.get('master_key')}",
        ]
        return 0, "\n".join(lines)

    def _deploy_integrate(self, options):
        cfg = self._get_instance(options, [])
        host = options.get("host") or f"http://localhost:{cfg['port']}"
        path = integration.write_meilisearch_config(host, cfg.get("master_key", ""))
        return 0, f"已生成 meilisearch 插件配置: {path}\n({host}, key 已写入)"