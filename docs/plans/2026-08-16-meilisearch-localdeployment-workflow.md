> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


---
intent: 为 VUS 提供 meilisearch_localdeployment 功能拓展包，可在本机部署并管理 Meilisearch 搜索服务（优先 Docker、回退官方二进制），并深度集成到 meilisearch 插件。
success_criteria: 拓展可 build 成 .vux 包；支持 Docker/二进制两种部署；提供 安装/启动/停止/重启/状态/卸载/更新/日志/配置/集成 命令；多实例；`部署 集成` 生成 meilisearch config.json；单元测试覆盖平台解析/命令路由/config.json 生成/回退决策。
risk_level: medium
auto_approve: true
branch: master
worktree: false
---

## Steps

- [ ] **Step 1: 创建拓展包骨架**
action: 创建目录 plugins/func/meilisearch_localdeployment/，写入 vux.json（名称 "Meilisearch本地部署拓展"、版本 1.0.0、作者 rtcn_0523@qq.com、入口 __init__.py、依赖 {}、Python依赖 {}）、vux依赖.txt（内容 "meilisearch.vux >= 1.0.0"）、vuxpy依赖.txt（内容 "# 仅使用 Python 标准库"）。
verify:
  type: artifact
  path: plugins/func/meilisearch_localdeployment
  assert:
    kind: exists

- [ ] **Step 2: 实现 platform.py 平台解析**
action: 在 plugins/func/meilisearch_localdeployment/platform.py 实现：detect_os()（linux/darwin/windows）、detect_arch()（x86_64/arm64/arm32/x86）、is_alpine()（检测 /etc/alpine-release 存在）、resolve_asset(version)（返回 (os,arch,asset_name) 或 None）。规则：Linux x86_64→meilisearch-linux-amd64；Linux arm64→meilisearch-linux-aarch64；Linux arm32+Alpine→meilisearch-linux-armv7；Linux arm32+非Alpine→返回特殊标记 BUILD_FROM_SOURCE；macOS→meilisearch-darwin-amd64/aarch64；Windows→meilisearch-windows-amd64.exe。暴露常量 BUILD_FROM_SOURCE = "build-from-source"。提供函数 get_latest_version()（用 urllib 请求 GitHub API 最新 release，失败返回 None）。
verify:
  type: shell
  command: python3 -c "import sys; sys.path.insert(0,'plugins/func/meilisearch_localdeployment'); import platform as p; print(p.detect_os(), p.detect_arch())"

- [ ] **Step 3: 实现 instance.py 实例管理**
action: 在 plugins/func/meilisearch_localdeployment/instance.py 实现 InstanceManager：目录 base=~/.vus/plugins/meilisearch_localdeployment/，数据目录 data_dir=~/.vus/data/meilisearch/<name>/。方法：list_instances()、load_instance(name)、save_instance(name, cfg)、delete_instance(name)、instance_path(name)、pid_path(name)、log_path(name)。cfg 含 name/port/primary_key/master_key/data_dir/version/deploy_mode。默认实例 default 端口 7700。master_key 未提供时用 secrets.token_hex(16) 生成。
verify:
  type: shell
  command: python3 -c "import sys; sys.path.insert(0,'plugins/func/meilisearch_localdeployment'); import instance; print(instance.InstanceManager('').instance_path.__name__)"

- [ ] **Step 4: 实现 deployer.py 部署器**
action: 在 plugins/func/meilisearch_localdeployment/deployer.py 实现：抽象类 Deployer（install/start/stop/restart/status/uninstall/update/run_log 抽象方法）。DockerDeployer：docker_available()（docker --version 非零则 False）、install 拉取 getmeili/meilisearch:vX、start 用 docker run -d -p 端口:7700 -e MEILI_MASTER_KEY -v 数据目录:/data.meli、stop/restart/status 用 docker 命令、run_log 用 docker logs。BinaryDeployer：install 从 GitHub releases 下载 resolve_asset 名并 chmod +x，存 ~/.vus/bin/meilisearch-<version>；start 用 subprocess.Popen 启动记录 PID 到 pid 文件、stdout/stderr 到 log 文件；stop 读 PID kill；status 检查进程存活；run_log 读 log 文件。deploy_mode 通过 detect_deployer() 返回（优先 Docker，否则 Binary，arm32 非 Alpine 返回 BUILD_FROM_SOURCE 错误）。
verify:
  type: shell
  command: python3 -c "import sys; sys.path.insert(0,'plugins/func/meilisearch_localdeployment'); import deployer; print(deployer.DockerDeployer, deployer.BinaryDeployer)"

- [ ] **Step 5: 实现 integration.py 深度集成**
action: 在 plugins/func/meilisearch_localdeployment/integration.py 实现 write_meilisearch_config(host, api_key)：写入 ~/.vus/plugins/meilisearch/config.json，内容 {"host": host, "api_key": api_key}，创建父目录。返回写入路径。
verify:
  type: shell
  command: python3 -c "import sys; sys.path.insert(0,'plugins/func/meilisearch_localdeployment'); import integration; print(hasattr(integration,'write_meilisearch_config'))"

- [ ] **Step 6: 实现 __init__.py 命令路由**
action: 在 plugins/func/meilisearch_localdeployment/__init__.py 实现插件类 MeilisearchLocalDeploymentPlugin，暴露 run(rest, options) 返回 (code, message)。解析 rest 首 token 为子命令（部署/实例），部署下再解析二级子命令（信息/安装/启动/停止/重启/状态/更新/卸载/日志/配置/集成），实例下（列表/创建/删除）。options 支持 --版本/--实例/--端口/--主键/--master-key/--数据目录。集成命令调用 integration.write_meilisearch_config。所有命令返回 (0, 中文消息) 或 (0, "错误: ...")。
verify:
  type: shell
  command: python3 -c "import sys; sys.path.insert(0,'plugins/func/meilisearch_localdeployment'); from __init__ import MeilisearchLocalDeploymentPlugin; p=MeilisearchLocalDeploymentPlugin(); print(p.run(['部署','信息'],{}))"

- [ ] **Step 7: 编写单元测试**
action: 在 plugins/func/meilisearch_localdeployment/tests/test_localdeploy.py 编写测试：mock platform.detect_os/detect_arch/is_alpine 覆盖 Linux x86_64/arm64/Alpine arm32/非Alpine arm32/Windows 的 resolve_asset 决策；test_resolve_asset_build_from_source 断言非Alpine arm32 返回 BUILD_FROM_SOURCE；test_detect_deployer 用 mock docker 可用/缺失断言 Docker/Binary/BUILD_FROM_SOURCE；test_write_config 用 tempfile 远端替代 ~/.vus 断言 config.json 内容；test_run_route 断言 run(['部署','信息']) 返回 code 0。用 mock.patch 隔离网络与文件系统。
verify:
  type: shell
  command: cd plugins/func/meilisearch_localdeployment && python3 -m pytest tests/ -v

- [ ] **Step 8: 运行全部测试并打包**
action: 在 plugins/func/meilisearch_localdeployment 下运行 python3 -m pytest tests/ -v 确认全部通过；然后 cd /workspace/vus && python3 scripts/vux_plugin_manager.py build plugins/func/meilisearch_localdeployment 生成 .vux 包文件。
verify:
  type: shell
  command: cd /workspace/vus && python3 scripts/vux_plugin_manager.py build plugins/func/meilisearch_localdeployment

- [ ] **Step 9: 提交代码走查（gate: human）**
action: 由用户走查 plugins/func/meilisearch_localdeployment/ 全部新增文件与设计一致性：命令面、平台解析、集成逻辑。确认无误后提交。
loop: until 用户确认代码走查通过
max_iterations: 3
verify: git status --short
gate: human

- [ ] **Step 10: 提交并推送**
action: 在 /workspace/vus 下 git add plugins/func/meilisearch_localdeployment docs/designs/2026-08-16-meilisearch-localdeployment-design.md docs/plans/2026-08-16-meilisearch-localdeployment-workflow.md，提交信息 "feat: 新增 Meilisearch 本地部署拓展插件（Docker/二进制部署、多实例、深度集成）"，然后 git push origin master。删除生成的 .vux 构建产物。
verify:
  type: shell
  command: cd /workspace/vus && git log --oneline -1