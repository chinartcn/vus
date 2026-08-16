---
intent: 为 VUS 插件系统开发第一个正式功能插件——封装 Meilisearch 全文搜索引擎，通过中文子命令提供索引管理、文档管理、企业级搜索、同义词与设置管理能力。
success_criteria: vux_plugin_manager.py 可安装/列出/运行 meilisearch 插件；中文子命令覆盖健康统计、索引 CRUD、文档 CRUD、搜索（含筛选/排序/高亮/分面）、同义词、设置；连接配置支持环境变量优先、配置文件兜底；单元测试全部通过。
risk_level: low
auto_approve: true
branch: master
worktree: false
---

## Steps

- [ ] **Step 1: 创建插件目录与元数据文件**
action: 创建目录 plugins/func/meilisearch/，写入 vux.json（名称"Meilisearch搜索"，版本"1.0.0"，作者"rtcn_0523@qq.com"，描述，最低VUS版本，Python依赖含 meilisearch）和 vuxpy依赖.txt（内容: meilisearch），并创建占位 __init__.py。
verify:
  type: artifact
  path: plugins/func/meilisearch
  assert:
    kind: matches-glob
    value: "*.json"

- [ ] **Step 2: 实现连接配置解析**
action: 在 plugins/func/meilisearch/__init__.py 的 init() 中实现 _load_config()：依次读取环境变量 VUS_MEILI_HOST/VUS_MEILI_API_KEY → 配置文件 ~/.vus/plugins/meilisearch/config.json（含 host/api_key 字段）→ 默认 http://localhost:7700。建立 meilisearch.Client 实例并保存为 self._client。若配置缺失 host，返回非 0。同时实现 _find_vux_config() 辅助函数（读取 config.json 并兼容中英文键 host/api_key）。
verify:
  type: artifact
  path: plugins/func/meilisearch/__init__.py
  assert:
    kind: exists

- [ ] **Step 3: 实现中文子命令解析器**
action: 在 __init__.py 实现 _parse_command(line)，将输入字符串 split() 为 token 列表，解析 "--xxx" 选项及其值（支持 --筛选/--排序/--高亮/--分面/--限制/--索引/--主键/--文件/--json），返回 (args, options) 字典。实现 run() 入口：读取环境变量 VUS_BINARY 与 api，调用 _parse_command，按首 token 分发到 health_/index_/doc_/search_/synonym_/settings_ 前缀处理方法，未知子命令返回错误信息。定义 _get_index(name) 帮助函数返回 client.index(train)。
verify:
  type: artifact
  path: plugins/func/meilisearch/__init__.py
  assert:
    kind: exists

- [ ] **Step 4: 实现健康与统计命令**
action: 实现 cmd_状态/健康/统计：调用 client.health()、client.get_indexes_stats()，将结果以 JSON 格式返回输出字符串。处理连接异常返回可读错误。
verify:
  type: artifact
  path: plugins/func/meilisearch/__init__.py
  assert:
    kind: exists

- [ ] **Step 5: 实现索引管理命令**
action: 实现 cmd_索引 子命令路由：列表（client.get_indexes()）、创建 <名> [--主键 xxx]（client.create_index）、删除 <名>（client.delete_index）、设置 <名>（client.get_index_settings / update_index_settings，支持 --筛选属性/--排序属性/--分面数量 等）。每个操作结果格式化为 JSON 或中文描述返回。
verify:
  type: artifact
  path: plugins/func/meilisearch/__init__.py
  assert:
    kind: exists

- [ ] **Step 6: 实现文档管理命令**
action: 实现 cmd_文档 子命令路由：添加 <索引> --文件 xxx.json 或 --json '[...]'（index.add_documents，支持 --主键 指定主键）、更新 <索引>（index.update_documents）、删除 <索引> <id>（index.delete_document）、获取 <索引> <id>（index.get_document）。--文件 时读取 JSON 文件内容。结果返回任务 uid 或文档内容。
verify:
  type: artifact
  path: plugins/func/meilisearch/__init__.py
  assert:
    kind: exists

- [ ] **Step 7: 实现企业级搜索命令**
action: 实现 cmd_搜索 <关键词> 命令：默认索引由 --索引 <名> 指定（缺省报错提示），组装 search 参数（--筛选→filter、--排序→sort、--高亮→attributes_to_highlight、--分面→facets、--限制→limit），调用 index.search(query, **params)，返回命中数、耗时、每条命中的高亮字段与文档内容。
verify:
  type: artifact
  path: plugins/func/meilisearch/__init__.py
  assert:
    kind: exists

- [ ] **Step 8: 实现同义词与设置命令**
action: 实现 cmd_同义词 子命令路由：设置 <索引> --词1,词2;词3,词4（解析分号分隔的组）、获取 <索引>（返回同义词组）。确保调用 index.update_synonyms() / 从 settings 读取。
verify:
  type: artifact
  path: plugins/func/meilisearch/__init__.py
  assert:
    kind: exists

- [ ] **Step 9: 编写单元测试**
action: 创建 plugins/func/meilisearch/tests/test_meilisearch.py，用 unittest.mock 隔离 meilisearch SDK：测试 _parse_command 的选项解析；mock Client 验证 health/index_create/index_add_documents/search 的命令路由与参数传递；测试缺参、未知子命令、非法 --限制 值的错误处理。同时 mock 环境变量与 config.json 验证 _load_config 的优先级（环境变量 > 配置文件 > 默认值）。
verify:
  type: shell
  command: cd plugins/func/meilisearch && python3 -m pytest tests/ -v

- [ ] **Step 10: 验证插件生命周期**
action: 依次执行：python3 scripts/vux_plugin_manager.py info plugins/func/meilisearch、python3 scripts/vux_plugin_manager.py build plugins/func/meilisearch、python3 scripts/vux_plugin_manager.py list、python3 scripts/vux_plugin_manager.py run meilisearch 状态（确认在无 Meilisearch 实例时返回可读错误而非崩溃）。确保 build 生成 .vux 文件。
verify:
  type: shell
  command: python3 scripts/vux_plugin_manager.py info plugins/func/meilisearch

- [ ] **Step 11: 更新生态文档**
action: 在 docs/ECOSYSTEM.md 章节四新增"Meilisearch 搜索插件"小节，列出插件名、中文命令一览、连接配置方式、安装运行示例（vux install meilisearch、vux run 搜索 hello）。不破坏现有章节结构。
verify:
  type: artifact
  path: docs
  assert:
    kind: matches-glob
    value: "ECOSYSTEM.md"