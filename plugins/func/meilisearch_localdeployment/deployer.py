"""部署器：DockerDeployer（优先）与 BinaryDeployer（回退）。

部署方式决策 detect_deployer()：
- 检测到 Docker 可用 -> DockerDeployer
- 无 Docker 且平台可下载二进制 -> BinaryDeployer
- 无 Docker 且 arm32 非 Alpine -> BUILD_FROM_SOURCE（提示源码构建）
"""

import os
import shutil
import subprocess
import urllib.request
from pathlib import Path

import target

BIN_DIR = Path("~/.vus/bin").expanduser()
DOCKER_IMAGE = "getmeili/meilisearch"


def docker_available():
    """检测 Docker CLI 是否存在（不验证 daemon）。"""
    return shutil.which("docker") is not None


class Deployer:
    """部署器抽象基类。"""

    mode = "abstract"

    def install(self, version, cfg):
        raise NotImplementedError

    def start(self, cfg):
        raise NotImplementedError

    def stop(self, cfg):
        raise NotImplementedError

    def restart(self, cfg):
        raise NotImplementedError

    def status(self, cfg):
        raise NotImplementedError

    def uninstall(self, cfg):
        raise NotImplementedError

    def update(self, version, cfg):
        raise NotImplementedError

    def run_log(self, cfg, lines=50):
        raise NotImplementedError


class DockerDeployer(Deployer):
    """通过 Docker 容器部署 Meilisearch。"""

    mode = "docker"

    def _container(self, cfg):
        return f"vus-meilisearch-{cfg['name']}"

    def install(self, version, cfg):
        image = f"{DOCKER_IMAGE}:v{version.lstrip('v')}"
        return subprocess.run(["docker", "pull", image], capture_output=True, text=True)

    def start(self, cfg):
        name = self._container(cfg)
        image = f"{DOCKER_IMAGE}:v{cfg.get('version', 'latest').lstrip('v')}"
        data_dir = Path(cfg["data_dir"])
        data_dir.mkdir(parents=True, exist_ok=True)
        cmd = [
            "docker", "run", "-d", "--name", name,
            "-p", f"{cfg['port']}:7700",
            "-v", f"{data_dir}:/data.meli",
            "-e", f"MEILI_MASTER_KEY={cfg['master_key']}",
            image,
        ]
        return subprocess.run(cmd, capture_output=True, text=True)

    def stop(self, cfg):
        return subprocess.run(["docker", "stop", self._container(cfg)],
                              capture_output=True, text=True)

    def restart(self, cfg):
        return subprocess.run(["docker", "restart", self._container(cfg)],
                              capture_output=True, text=True)

    def status(self, cfg):
        name = self._container(cfg)
        r = subprocess.run(["docker", "ps", "-a", "--filter",
                            f"name=^{name}$", "--format", "{{.Names}}\t{{.Status}}"],
                           capture_output=True, text=True)
        return r.stdout.strip()

    def uninstall(self, cfg):
        self.stop(cfg)
        return subprocess.run(["docker", "rm", self._container(cfg)],
                              capture_output=True, text=True)

    def update(self, version, cfg):
        self.stop(cfg)
        subprocess.run(["docker", "rm", self._container(cfg)], capture_output=True, text=True)
        return self.install(version, cfg)

    def run_log(self, cfg, lines=50):
        r = subprocess.run(["docker", "logs", "--tail", str(lines), self._container(cfg)],
                           capture_output=True, text=True)
        return r.stdout + r.stderr


class BinaryDeployer(Deployer):
    """下载官方二进制并作为子进程管理。"""

    mode = "binary"

    def _binary(self, version, asset=None):
        asset = asset or target.resolve_asset()
        return BIN_DIR / f"meilisearch-{version.lstrip('v')}-{asset}"

    def install(self, version, cfg):
        asset = target.resolve_asset()
        if asset == target.BUILD_FROM_SOURCE:
            return None, "当前平台（arm32 非 Alpine Linux）无官方二进制，请从源码构建 Meilisearch 后手动放置。"

        url = target.download_url(version, asset)
        dest = self._binary(version, asset)
        BIN_DIR.mkdir(parents=True, exist_ok=True)
        try:
            with urllib.request.urlopen(url, timeout=60) as resp:
                data = resp.read()
        except Exception as e:
            return None, f"下载失败: {url}\n原因: {e}"

        tmp = dest.with_suffix(".tmp")
        tmp.write_bytes(data)
        os.chmod(tmp, 0o755)
        tmp.replace(dest)
        return dest, None

    def start(self, cfg):
        binary = self._binary(cfg.get("version", "latest"))
        if not binary.is_file():
            return None, f"二进制不存在: {binary}，请先执行 部署 安装"

        data_dir = Path(cfg["data_dir"])
        data_dir.mkdir(parents=True, exist_ok=True)
        log_path = cfg.get("_log_path", "")
        log_fh = open(log_path, "a", encoding="utf-8") if log_path else subprocess.DEVNULL

        cmd = [
            str(binary),
            "--http-addr", f"0.0.0.0:{cfg['port']}",
            "--db-path", str(data_dir),
        ]
        if cfg.get("master_key"):
            cmd += ["--master-key", cfg["master_key"]]

        env = os.environ.copy()
        if cfg.get("cors_origins"):
            env["MEILI_HTTP_CORS_ORIGIN"] = cfg["cors_origins"]

        # stdin 重定向到 DEVNULL（避免父进程退出后 stdin EOF 导致子进程退出），
        # 并 start_new_session 脱离父进程组，防止沙箱/父进程退出时连带清理 meilisearch。
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                stdout=log_fh, stderr=subprocess.STDOUT,
                                env=env, start_new_session=True)
        cfg["_pid"] = proc.pid
        return proc, None

    def stop(self, cfg):
        pid = cfg.get("_pid")
        if not pid:
            return False, "未找到 PID"
        try:
            os.kill(pid, 15)
            return True, None
        except ProcessLookupError:
            return True, "进程已不存在"
        except PermissionError as e:
            return False, str(e)

    def restart(self, cfg):
        self.stop(cfg)
        return self.start(cfg)

    def status(self, cfg):
        pid = cfg.get("_pid")
        if not pid:
            return "未运行（无 PID）"
        try:
            os.kill(pid, 0)
            return f"运行中 (PID {pid})"
        except ProcessLookupError:
            return "已停止"
        except PermissionError:
            return f"存在 (PID {pid})"

    def uninstall(self, cfg):
        self.stop(cfg)
        binary = self._binary(cfg.get("version", "latest"))
        if binary.is_file():
            binary.unlink()
            return f"已删除二进制: {binary}"
        return "二进制不存在，无需删除"

    def update(self, version, cfg):
        self.stop(cfg)
        return self.install(version, cfg)

    def run_log(self, cfg, lines=50):
        log_path = Path(cfg.get("_log_path", ""))
        if not log_path.is_file():
            return "(无日志)"
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            return "".join(f.readlines()[-lines:])


def detect_deployer():
    """返回部署器实例或 BUILD_FROM_SOURCE。

    Returns:
        Deployer 实例；arm32 非 Alpine 且无 Docker 时返回 target.BUILD_FROM_SOURCE 字符串。
    """
    if docker_available():
        return DockerDeployer()
    asset = target.resolve_asset()
    if asset == target.BUILD_FROM_SOURCE:
        return target.BUILD_FROM_SOURCE
    return BinaryDeployer()