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


def _sample_cfg(**overrides):
    """构造一份最小实例配置。"""
    cfg = {
        "name": "default",
        "port": 7700,
        "master_key": "mk",
        "version": "1.8.0",
        "data_dir": str(Path(tempfile.gettempdir()) / "vus_ms_test_data"),
    }
    cfg.update(overrides)
    return cfg


class TestDockerDeployer(unittest.TestCase):
    """Docker 部署器命令构建。"""

    def setUp(self):
        self.d = deployer.DockerDeployer()

    @mock.patch("deployer.subprocess.run")
    def test_install_pull_image(self, run):
        run.return_value = mock.Mock(returncode=0, stderr="")
        self.d.install("1.8.0", _sample_cfg())
        run.assert_called_once_with(
            ["docker", "pull", "getmeili/meilisearch:v1.8.0"],
            capture_output=True, text=True)

    @mock.patch("deployer.subprocess.run")
    def test_start_run_container(self, run):
        run.return_value = mock.Mock(returncode=0, stderr="")
        self.d.start(_sample_cfg(name="dev", port=7701, data_dir="/tmp/x"))
        cmd = run.call_args.args[0]
        self.assertEqual(cmd[0], "docker")
        self.assertIn("vus-meilisearch-dev", cmd)      # 容器名
        self.assertIn("7701:7700", cmd)                # 端口映射
        self.assertIn("MEILI_MASTER_KEY=mk", cmd)      # master key 注入
        self.assertIn("/tmp/x:/data.meli", cmd)        # 数据卷挂载

    @mock.patch("deployer.subprocess.run")
    def test_stop_uses_container_name(self, run):
        run.return_value = mock.Mock(returncode=0, stderr="")
        self.d.stop(_sample_cfg(name="dev"))
        self.assertEqual(run.call_args.args[0],
                         ["docker", "stop", "vus-meilisearch-dev"])

    @mock.patch("deployer.subprocess.run")
    def test_status_filters_by_name(self, run):
        run.return_value = mock.Mock(
            stdout="vus-meilisearch-dev\tUp 2 seconds", stderr="")
        out = self.d.status(_sample_cfg(name="dev"))
        self.assertEqual(out, "vus-meilisearch-dev\tUp 2 seconds")
        self.assertIn("name=^vus-meilisearch-dev$", run.call_args.args[0])

    @mock.patch("deployer.subprocess.run")
    def test_uninstall_stops_and_removes(self, run):
        run.return_value = mock.Mock(returncode=0, stderr="")
        self.d.uninstall(_sample_cfg(name="dev"))
        calls = [c.args[0] for c in run.call_args_list]
        self.assertIn(["docker", "stop", "vus-meilisearch-dev"], calls)
        self.assertIn(["docker", "rm", "vus-meilisearch-dev"], calls)


class TestBinaryDeployer(unittest.TestCase):
    """Binary 部署器：下载、启动、停止、状态、日志。"""

    def setUp(self):
        self.d = deployer.BinaryDeployer()
        patcher = mock.patch.object(target, "resolve_asset",
                                    return_value="meilisearch-linux-amd64")
        self.mock_asset = patcher.start()
        self.addCleanup(patcher.stop)

    @mock.patch("deployer.os.chmod")
    @mock.patch("deployer.urllib.request.urlopen")
    def test_install_downloads_and_chmod(self, urlopen, chmod):
        urlopen.return_value.__enter__.return_value.read.return_value = b"BIN"
        dest, err = self.d.install("1.8.0", _sample_cfg())
        self.assertIsNone(err)
        self.assertTrue(dest.name.startswith("meilisearch-1.8.0"))
        urlopen.assert_called_once()
        chmod.assert_called_once()

    @mock.patch("deployer.urllib.request.urlopen",
                side_effect=OSError("网络不可达"))
    def test_install_download_failure_returns_error(self, _):
        dest, err = self.d.install("1.8.0", _sample_cfg())
        self.assertIsNone(dest)
        self.assertIn("下载失败", err)

    @mock.patch.object(target, "resolve_asset",
                       return_value=target.BUILD_FROM_SOURCE)
    def test_install_build_from_source_message(self, _):
        dest, err = self.d.install("1.8.0", _sample_cfg())
        self.assertIsNone(dest)
        self.assertIn("从源码构建", err)

    @mock.patch("deployer.subprocess.Popen")
    def test_start_missing_binary_returns_error(self, popen):
        missing = Path(tempfile.gettempdir()) / "vus_missing_meili_bin"
        with mock.patch.object(self.d, "_binary", return_value=missing):
            proc, err = self.d.start(_sample_cfg())
        self.assertIsNone(proc)
        self.assertIn("二进制不存在", err)
        popen.assert_not_called()

    @mock.patch("deployer.subprocess.Popen")
    def test_start_spawns_with_master_key(self, popen):
        popen.return_value.pid = 4242
        binary = self.d._binary("1.8.0")
        binary.write_bytes(b"x")
        binary.chmod(0o755)
        self.addCleanup(binary.unlink)
        proc, err = self.d.start(_sample_cfg(version="1.8.0"))
        self.assertIsNone(err)
        self.assertEqual(proc.pid, 4242)
        cmd = popen.call_args.args[0]
        self.assertIn("--master-key", cmd)
        self.assertIn("mk", cmd)

    def test_stop_missing_pid(self):
        ok, err = self.d.stop(_sample_cfg())
        self.assertFalse(ok)
        self.assertIn("未找到 PID", err)

    @mock.patch("deployer.os.kill")
    def test_stop_kills_pid(self, kill):
        ok, err = self.d.stop(_sample_cfg(_pid=123))
        self.assertTrue(ok)
        self.assertIsNone(err)
        kill.assert_called_once_with(123, 15)

    def test_status_no_pid(self):
        self.assertEqual(self.d.status(_sample_cfg()), "未运行（无 PID）")

    @mock.patch("deployer.os.kill")
    def test_status_running(self, kill):
        kill.return_value = None  # 0
        self.assertEqual(self.d.status(_sample_cfg(_pid=1)), "运行中 (PID 1)")

    @mock.patch("deployer.os.kill", side_effect=ProcessLookupError)
    def test_status_stopped(self, _):
        self.assertEqual(self.d.status(_sample_cfg(_pid=1)), "已停止")

    def test_run_log_reads_last_lines(self):
        log = Path(tempfile.gettempdir()) / "vus_test.log"
        log.write_text("line1\nline2\nline3\n", encoding="utf-8")
        self.addCleanup(log.unlink)
        out = self.d.run_log(_sample_cfg(_log_path=str(log)), lines=2)
        self.assertEqual(out, "line2\nline3\n")

    def test_run_log_missing_file(self):
        self.assertEqual(self.d.run_log(_sample_cfg(_log_path="/nonexist.log")),
                         "(无日志)")


class TestPluginRouteExtended(unittest.TestCase):
    """补齐命令路由：安装/启动/停止/重启/状态/更新/卸载/日志/配置。"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.plugin = MeilisearchLocalDeploymentPlugin(
            mgr=inst.InstanceManager(
                plugin_dir=str(Path(self.tmp.name) / "plugins"),
                data_root=str(Path(self.tmp.name) / "data"),
            ))

    def tearDown(self):
        self.tmp.cleanup()

    def _no_docker(self):
        p1 = mock.patch.object(deployer, "docker_available", return_value=False)
        p2 = mock.patch.object(target, "resolve_asset",
                               return_value="meilisearch-linux-amd64")
        return p1, p2

    def test_deploy_install_missing_version_for_update(self):
        code, msg = self.plugin.run(None, "部署 更新")
        self.assertEqual(code, 0)
        self.assertIn("--版本", msg)

    def test_deploy_unknown_subcommand(self):
        code, msg = self.plugin.run(None, "部署 神秘操作")
        self.assertEqual(code, 0)
        self.assertIn("未知部署子命令", msg)

    def test_deploy_log_binary_no_log(self):
        p1, p2 = self._no_docker()
        with p1, p2:
            code, msg = self.plugin.run(None, "部署 日志")
        self.assertEqual(code, 0)
        self.assertIn("日志", msg)

    def test_deploy_config_shows_fields(self):
        code, msg = self.plugin.run(None, "部署 配置 --端口 7799")
        self.assertEqual(code, 0)
        self.assertIn("7799", msg)
        self.assertIn("端口", msg)

    @mock.patch("deployer.subprocess.Popen")
    def test_deploy_start_binary(self, popen):
        popen.return_value.pid = 777
        fake_bin = Path(tempfile.gettempdir()) / "vus_fake_meili"
        fake_bin.write_bytes(b"x")
        popen.return_value.pid = 777
        self.addCleanup(fake_bin.unlink)
        p1, p2 = self._no_docker()
        with p1, p2, mock.patch.object(deployer.BinaryDeployer, "_binary",
                                       return_value=fake_bin):
            code, msg = self.plugin.run(None, "部署 启动 --端口 7703")
        self.assertEqual(code, 0)
        self.assertIn("777", msg)

    @mock.patch("deployer.subprocess.run")
    def test_deploy_status_docker(self, run):
        run.return_value = mock.Mock(stdout="Up 1 second", stderr="")
        with mock.patch.object(deployer, "docker_available", return_value=True):
            code, msg = self.plugin.run(None, "部署 状态")
        self.assertEqual(code, 0)
        self.assertIn("Up 1 second", msg)


if __name__ == "__main__":
    unittest.main()