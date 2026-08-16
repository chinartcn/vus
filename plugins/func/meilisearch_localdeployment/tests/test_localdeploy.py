"""Meilisearch 本地部署拓展单元测试。

覆盖：平台解析（含 arm32 非 Alpine 降级）、部署方式决策、config.json 生成、
命令路由。用 mock 隔离网络与文件系统。
"""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import deployer
import instance as inst
import integration
import target
from __init__ import MeilisearchLocalDeploymentPlugin


class TestResolveAsset(unittest.TestCase):
    """平台/架构 -> 二进制资产选择。"""

    def test_linux_x86_64(self):
        self.assertEqual(target.resolve_asset("linux", "x86_64"), "meilisearch-linux-amd64")

    def test_linux_arm64(self):
        self.assertEqual(target.resolve_asset("linux", "arm64"), "meilisearch-linux-aarch64")

    @mock.patch.object(target, "is_alpine", return_value=True)
    def test_linux_arm32_alpine(self, _):
        self.assertEqual(target.resolve_asset("linux", "arm32"), "meilisearch-linux-armv7")

    @mock.patch.object(target, "is_alpine", return_value=False)
    def test_linux_arm32_non_alpine_build_from_source(self, _):
        self.assertEqual(target.resolve_asset("linux", "arm32"), target.BUILD_FROM_SOURCE)

    def test_darwin(self):
        self.assertEqual(target.resolve_asset("darwin", "x86_64"), "meilisearch-darwin-x86_64")
        self.assertEqual(target.resolve_asset("darwin", "arm64"), "meilisearch-darwin-arm64")

    def test_windows(self):
        self.assertEqual(target.resolve_asset("windows", "x86_64"), "meilisearch-windows-amd64.exe")

    def test_unknown_returns_none(self):
        self.assertIsNone(target.resolve_asset("linux", "unknown"))


class TestDownloadUrl(unittest.TestCase):
    def test_url_format(self):
        self.assertIn("v1.8.0/meilisearch-linux-amd64",
                      target.download_url("1.8.0", "meilisearch-linux-amd64"))


class TestDetectDeployer(unittest.TestCase):
    """部署方式决策：有/无 Docker，arm32 降级。"""

    @mock.patch.object(deployer, "docker_available", return_value=True)
    def test_docker_available_uses_docker(self, _):
        d = deployer.detect_deployer()
        self.assertEqual(d.mode, "docker")

    @mock.patch.object(deployer, "docker_available", return_value=False)
    @mock.patch.object(target, "resolve_asset", return_value="meilisearch-linux-amd64")
    def test_no_docker_uses_binary(self, _a, _b):
        d = deployer.detect_deployer()
        self.assertEqual(d.mode, "binary")

    @mock.patch.object(deployer, "docker_available", return_value=False)
    @mock.patch.object(target, "resolve_asset", return_value=target.BUILD_FROM_SOURCE)
    def test_no_docker_arm32_build_from_source(self, _a, _b):
        self.assertEqual(deployer.detect_deployer(), target.BUILD_FROM_SOURCE)


class TestIntegration(unittest.TestCase):
    """深度集成 config.json 生成。"""

    def test_write_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "meilisearch" / "config.json"
            written = integration.write_meilisearch_config(
                "http://localhost:7700", "secret-key", config_path=str(cfg_path))
            self.assertEqual(Path(written).resolve(), cfg_path.resolve())
            data = json.loads(cfg_path.read_text(encoding="utf-8"))
            self.assertEqual(data["host"], "http://localhost:7700")
            self.assertEqual(data["api_key"], "secret-key")


class TestInstanceManager(unittest.TestCase):
    """实例管理：保存/加载/列表/删除。"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.mgr = inst.InstanceManager(
            plugin_dir=str(Path(self.tmp.name) / "plugins"),
            data_root=str(Path(self.tmp.name) / "data"),
        )

    def tearDown(self):
        self.tmp.cleanup()

    def test_save_and_load_generates_master_key(self):
        self.mgr.save_instance("default", {"port": 7700})
        cfg = self.mgr.load_instance("default")
        self.assertEqual(cfg["port"], 7700)
        self.assertTrue(cfg["master_key"])  # 生成了随机 key

    def test_list_and_delete(self):
        self.mgr.save_instance("a", {})
        self.mgr.save_instance("b", {})
        self.assertIn("a", self.mgr.list_instances())
        self.assertIn("b", self.mgr.list_instances())
        self.mgr.delete_instance("a")
        self.assertNotIn("a", self.mgr.list_instances())


class TestPluginRoute(unittest.TestCase):
    """命令路由与参数解析。"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.plugin = MeilisearchLocalDeploymentPlugin(
            mgr=inst.InstanceManager(
                plugin_dir=str(Path(self.tmp.name) / "plugins"),
                data_root=str(Path(self.tmp.name) / "data"),
            ))

    def tearDown(self):
        self.tmp.cleanup()

    def test_usage_on_empty(self):
        code, msg = self.plugin.run(None, "")
        self.assertEqual(code, 0)
        self.assertIn("用法", msg)

    def test_unknown_root(self):
        code, msg = self.plugin.run(None, "foo bar")
        self.assertEqual(code, 0)
        self.assertIn("错误", msg)

    def _no_docker(self):
        patch_docker = mock.patch.object(deployer, "docker_available", return_value=False)
        patch_asset = mock.patch.object(target, "resolve_asset",
                                        return_value="meilisearch-linux-amd64")
        return patch_docker, patch_asset

    def test_deploy_info(self):
        p1, p2 = self._no_docker()
        with p1, p2:
            code, msg = self.plugin.run(None, "部署 信息")
        self.assertEqual(code, 0)
        self.assertIn("部署方式", msg)

    def test_instance_list_empty(self):
        code, msg = self.plugin.run(None, "实例 列表")
        self.assertEqual(code, 0)
        self.assertIn("暂无实例", msg)

    def test_instance_create_then_list(self):
        self.plugin.run(None, "实例 创建 dev --端口 7701")
        code, msg = self.plugin.run(None, "实例 列表")
        self.assertEqual(code, 0)
        self.assertIn("dev", msg)

    def test_integrate_writes_meilisearch_config(self):
        p1, p2 = self._no_docker()
        cfg_target = str(Path(self.tmp.name) / "meilisearch" / "config.json")
        with p1, p2, mock.patch.object(integration, "MEILISEARCH_CONFIG",
                                       Path(cfg_target)):
            code, msg = self.plugin.run(
                None, "部署 集成 --实例 dev --端口 7701 --master-key mk")
            self.assertEqual(code, 0)
        data = json.loads(Path(cfg_target).read_text(encoding="utf-8"))
        self.assertEqual(data["host"], "http://localhost:7701")
        self.assertEqual(data["api_key"], "mk")


if __name__ == "__main__":
    unittest.main()