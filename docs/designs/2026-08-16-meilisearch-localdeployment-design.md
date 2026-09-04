> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


---
design_type: feature
created_at: 2026-08-16
---

# Meilisearch 本地部署拓展设计

## Intent Contract

```
intent: 为 VUS 提供 meilisearch_localdeployment 功能拓展包，可在本机部署并管理
        Meilisearch 搜索服务（优先 Docker、回退官方二进制），并深度集成到
        meilisearch 插件（自动生成其 config.json 使其开箱即用）。
constraints:
  - 不得修改 meilisearch 插件现有命令与行为（向后兼容）
  - 拓展必须声明对 meilisearch.vux 的依赖并通过 vux依赖.txt 接入
  - 部署器必须处理 Docker 缺失时的二进制回退，及 arm32 非 Alpine 的降级提示
  - 不引入必须联网才能安装的构建步骤；二进制下载失败须给出可读错误
success_criteria:
  - 拓展作为 .vux 包可在官方插件仓库发布，`vus install meilisearch_localdeployment` 可用
  - 支持 Docker 部署与二进制部署两种模式，二者可切换
  - 提供 安装/启动/停止/重启/状态/卸载/更新/日志/配置/集成 中文子命令
  - 多实例管理（默认实例 default，可创建/删除命名实例）
  - `部署 集成` 能生成 meilisearch 插件的 config.json，使 meilisearch 命令无需手动配置即可连接本地实例
  - 单元测试覆盖平台解析、命令路由、config.json 生成、二进制回退决策
risk_level: medium
```

## Verification Contract

```
verify_steps:
  - run tests: cd plugins/func/meilisearch_localdeployment && python3 -m pytest tests/ -v
  - check: 平台解析函数对 Linux x86_64/arm64、Alpine arm32、非Alpine arm32、Windows 返回正确二进制选择
  - check: 非 Alpine arm32 返回"提示从源码构建"而非下载
  - check: 部署方式决策（有/无 Docker）正确选择 DockerDeployer / BinaryDeployer
  - check: 实例配置生成、PID/日志文件读写、config.json 生成内容正确
  - confirm: 全部测试通过；vus.json 含依赖声明；可 build 成 .vux 包
```

## Governance Contract

```
approval_gates:
  - 设计文档批准
  - 可执行工作流（writing-plans）批准
  - 实现提交前代码走查
rollback: 删除拓展目录与生成的 config.json；撤销 commit
ownership: 实现者负责；VUS Team 审核
```

## Scope

| 在范围内 | 不在范围内 |
|----------|-----------|
| 平台/架构检测与二进制选择 | 修改 meilisearch 插件源码/命令 |
| Docker 部署器（镜像拉取、容器启停） | 修改 vux_plugin_manager 依赖机制 |
| 二进制部署器（下载、进程启停、PID/日志） | 跨机远程部署/集群 |
| 多实例管理（创建/删除/列表） | 自动升级/滚动更新到最新版 |
| 配置管理（端口/主键/master key/数据目录） | 生产级高可用/备份恢复 |
| 深度集成（生成 meilisearch config.json） | Docker 镜像构建逻辑 |
| 平台解析与 arm32 降级提示 | |

## Decisions

| # | 决策 | 理由 | 备选与取舍 |
|---|------|------|-----------|
| 1 | 部署方式：优先 Docker，无 Docker 回退二进制 | 用户明确选择；Docker 环境隔离、跨平台一致 | 仅二进制（arm32 支持更好但需处理 glibc/musl） |
| 2 | 二进制来源：GitHub Releases，按 `${os}-${arch}` 命名 | 官方标准命名，便于版本化 | 官方 CDN 镜像（网络受限时作为回退） |
| 3 | arm32 支持：Alpine（musl）可下载；非 Alpine 提示源码构建 | 官方仅提供 musl armv7 二进制，无 glibc arm32 | 尝试 wine/交叉编译（过重，拒绝） |
| 4 | master key：首次部署自动生成随机 key 并持久化 | 安全默认，避免空 key；用户可用 --master-key 覆盖 | 默认空 key（不安全，拒绝） |
| 5 | 实例模型：命名实例，默认 default，数据目录 `~/.vus/data/meilisearch/<name>/` | 支持多实例且隔离数据 | 单实例（无法满足"多实例"要求） |
| 6 | 深度集成：`部署 集成` 写 `~/.vus/plugins/meilisearch/config.json` | 让 meilisearch 插件开箱即用 | 传输环境变量（不持久，拒绝） |
| 7 | Python 依赖：优先标准库 urllib，不强制 requests | 减少依赖，Termux 友好 | 引入 requests（多一个依赖，拒绝） |

## Surface

**包结构**（`plugins/func/meilisearch_localdeployment/`）：
- `__init__.py` — 命令路由入口，声明 `run(rest, options)` 接口
- `vux.json` — 元数据（名称/版本/作者/入口/依赖声明）
- `vux依赖.txt` — `meilisearch.vux >= 1.0.0`
- `vuxpy依赖.txt` — 空或仅标准库说明
- `platform.py` — OS/架构检测、二进制选择、arm32 降级决策
- `deployer.py` — `Deployer` 抽象 + `DockerDeployer` + `BinaryDeployer`
- `instance.py` — 实例状态、配置、PID/日志文件管理
- `integration.py` — 生成 meilisearch 插件 config.json
- `tests/` — 单元测试

**命令面**（中文子命令，与 meilisearch 插件风格一致）：
```
部署 信息                     显示部署方式、版本、实例列表
部署 安装 [--版本 vX.Y.Z]     安装/下载（Docker 镜像或二进制）
部署 启动 [--实例 n] [--端口 p] [--主键 f] [--master-key k] [--数据目录 d]
部署 停止 [--实例 n]
部署 重启 [--实例 n]
部署 状态 [--实例 n]
部署 更新 [--版本 vX.Y.Z]
部署 卸载 [--实例 n]
部署 日志 [--实例 n]
部署 配置 [--实例 n]
部署 集成 [--实例 n]          生成 meilisearch 插件 config.json
实例 列表 / 创建 / 删除
```

**存储**：
- 实例配置：`~/.vus/plugins/meilisearch_localdeployment/<name>.json`
- 数据目录：`~/.vus/data/meilisearch/<name>/`
- 集成产物：`~/.vus/plugins/meilisearch/config.json`（`{host, api_key}`）
- 二进制部署 PID/日志：`~/.vus/plugins/meilisearch_localdeployment/<name>.pid` / `.log`

**平台解析**（`platform.py`）：
- Linux x86_64 → `meilisearch-linux-amd64`
- Linux arm64 → `meilisearch-linux-aarch64`
- Linux arm32 + Alpine(musl) → `meilisearch-linux-armv7`
- Linux arm32 + 非 Alpine → 返回"提示从源码构建"
- macOS → `meilisearch-darwin-amd64` / `aarch64`
- Windows → `meilisearch-windows-amd64.exe`

## Risks & Open Questions

- **网络受限**：GitHub Releases 下载可能失败；需提供镜像回退与清晰错误提示。
- **Alpine 检测可靠性**：依赖 `/etc/alpine-release` 存在；边缘情况（容器内）可能误判。
- **Docker 可用性探测**：`docker --version` 存在但 daemon 未运行；启动时需二次确认。
- **端口冲突**：默认 7700 可能被占用；启动时检测并给出可读错误。
- **Open Question**：是否需要支持 `--no-docker` 强制二进制模式？默认按"自动检测"处理，后续按需扩展。