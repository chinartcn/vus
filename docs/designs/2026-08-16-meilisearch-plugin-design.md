> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


---
design_type: feature
created_at: 2026-08-16
---

# Meilisearch 插件设计

## Intent Contract

```
intent: 为 VUS 插件系统开发第一个正式功能插件——封装 Meilisearch 全文搜索引擎，
        通过中文子命令提供索引管理、文档管理、企业级搜索、同义词与设置管理能力。
constraints:
  - 不影响现有插件系统 API 与已有插件（示例插件、易语言插件）
  - 不强制要求 VUS 运行时具备 Python，插件作为独立 .vux 包分发
  - 完整 TUI 界面不在本次范围（后续实现）
success_criteria:
  - vux_plugin_manager.py 可安装/列出/运行 meilisearch 插件
  - 中文子命令覆盖：健康统计、索引 CRUD、文档 CRUD、搜索（含筛选/排序/高亮/分面）、同义词、设置
  - 连接配置支持环境变量优先、配置文件兜底
risk_level: low
```

## Verification Contract

```
verify_steps:
  - run tests: 编写 Python 单元测试（mock 或本地 Meilisearch），覆盖各子命令路由与参数解析
  - check: 通过 vux_plugin_manager.py install/info/list/run 验证插件生命周期
  - check: 对每个子命令做参数边界测试（缺参、非法值、未知子命令）
  - confirm: 单元测试全部通过；插件可被管理器正确加载并响应中文命令
```

## Governance Contract

```
approval_gates:
  - 设计文档审批（当前步骤）
  - 实现完成后代码审查（brooks-lint）
  - 合并到仓库前确认
rollback: 插件为独立目录 plugins/func/meilisearch/，删除该目录即可回滚；不影响编译器内核
ownership: 项目作者（rtcn_0523@qq.com）
```

## Scope

### In
| 模块 | 中文命令 | 说明 |
|------|---------|------|
| 健康/统计 | `状态` / `健康` / `统计` | 查询 Meilisearch 健康与统计信息 |
| 索引管理 | `索引 列表`、`索引 创建 <名> [--主键]`、`索引 删除 <名>`、`索引 设置 <名> ...` | 索引 CRUD 与设置读写 |
| 文档管理 | `文档 添加 <索引> --文件/--json`、`文档 更新 <索引> ...`、`文档 删除 <索引> <id>`、`文档 获取 <索引> <id>` | 文档增删改查 |
| 企业级搜索 | `搜索 <关键词> --索引 <名> [--筛选] [--排序] [--高亮] [--分面] [--限制]` | 全文搜索 + 筛选 + 排序 + 高亮 + 分面 |
| 同义词 | `同义词 设置 <索引> ...`、`同义词 获取 <索引>` | 同义词组管理 |
| 连接配置 | 环境变量 `VUS_MEILI_HOST`/`VUS_MEILI_API_KEY` + 配置文件 `config.json` | host/api key 解析 |

### Out
- 完整 TUI 管理界面（后续实现）
- 增量索引/同步任务调度
- 多租户/权限管理
- 与 VUS 语言内建函数（`meili_xxx`）的绑定（后续可考虑）

## Decisions

| # | 决策 | 选择 | 备选与理由 |
|---|------|------|-----------|
| 1 | 实现形式 | Python .vux 插件 | C .so 需手写 HTTP/JSON，成本高；Python SDK 成熟 |
| 2 | 命令接口 | 中文子命令路由 | 单一 run() 入口，靠命令字符串分发 |
| 3 | 命令语言 | 中文 | 契合 VUS 中文友好定位 |
| 4 | 连接配置 | 环境变量 + 配置文件 | 环境变量优先，配置文件兜底，默认 `http://localhost:7700` |
| 5 | 企业级搜索 | 内置筛选/排序/高亮/分面/同义词/设置 | 覆盖最常用搜索场景 |
| 6 | UI | 本次不做 | 完整 TUI 后续实现 |

## Surface

- **目录**：`plugins/func/meilisearch/`，含 `vux.json`、`__init__.py`、`vuxpy依赖.txt`
- **命令解析**：`run(api, input_data)` 将输入字符串按空格切片，首 token 为子命令，支持 `--xxx` 选项，`--文件` 支持从 JSON 文件读取文档
- **连接解析**：`init()` 读取环境变量 → 配置文件 → 默认值，建立 Meilisearch 客户端
- **文件触碰**：
  - `plugins/func/meilisearch/vux.json`
  - `plugins/func/meilisearch/__init__.py`
  - `plugins/func/meilisearch/vuxpy依赖.txt`
  - `plugins/func/meilisearch/tests/test_meilisearch.py`（单元测试）
  - `docs/ECOSYSTEM.md`（新增插件说明）

## Risks & Open Questions

- **依赖可用性**：`meilisearch` Python SDK 需 `pip install`，通过 `vuxpy依赖.txt` 声明，安装时自动处理
- **无 Meilisearch 实例时的验证**：单元测试用 mock 层隔离 SDK 调用，不依赖真实服务
- **中文命令与文件名冲突**：命令路由需与插件名/索引名区分，避免歧义
- **开放问题**：是否将插件发布到独立 `vus-plugins` 仓库（`REGISTRIES` 已预留），本次先落库到 vus 仓库