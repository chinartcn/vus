# Meilisearch 搜索插件使用文档

VUS 插件系统第一个正式功能插件，用 Python 封装 [Meilisearch](https://www.meilisearch.com/) 全文搜索引擎。通过中文子命令提供索引管理、文档管理、企业级搜索（筛选/排序/高亮/分面）、同义词与设置管理能力。

- **插件名**：`meilisearch`（显示名：`Meilisearch搜索`）
- **实现**：Python .vux 功能插件
- **依赖**：`meilisearch` Python SDK（≥ 0.30.0）
- **源码**：`plugins/func/meilisearch/`

---

## 1. 安装与运行

### 1.1 安装依赖

```bash
pip install meilisearch
```

### 1.2 安装插件

```bash
# 从 vus 仓库源码目录构建并安装
python3 scripts/vux_plugin_manager.py build plugins/func/meilisearch
python3 scripts/vux_plugin_manager.py install "./Meilisearch搜索-1.0.0.vux"

# 或者（若已发布到插件仓库）
vus vux install meilisearch
```

### 1.3 运行插件

```bash
python3 scripts/vux_plugin_manager.py run meilisearch "<中文命令>"
```

---

## 2. 连接配置

按优先级从高到低解析，未配置 `host` 时使用默认值：

| 优先级 | 来源 | 说明 |
|--------|------|------|
| 1 | 环境变量 | `VUS_MEILI_HOST`、`VUS_MEILI_API_KEY` |
| 2 | 配置文件 | `~/.vus/plugins/meilisearch/config.json`（键 `host` / `api_key`，兼容 `apiKey`） |
| 3 | 默认值 | `http://localhost:7700` |

环境变量方式：

```bash
export VUS_MEILI_HOST=https://search.example.com
export VUS_MEILI_API_KEY=your_master_key
```

配置文件方式：

```bash
mkdir -p ~/.vus/plugins/meilisearch
cat > ~/.vus/plugins/meilisearch/config.json <<'EOF'
{
    "host": "http://localhost:7700",
    "api_key": "your_master_key"
}
EOF
```

---

## 3. 命令参考

### 3.1 健康与统计

```
状态 / 健康                查询服务健康状态
统计                      查询数据库大小与索引概览
```

### 3.2 索引管理

```
索引 列表                                      列出所有索引
索引 创建 <名称> [--主键 <字段>]                创建索引
索引 删除 <名称>                                删除索引
索引 设置 <名称>                                读取索引当前设置
索引 设置 <名称> --筛选属性 a,b --排序属性 c --分面数量 100   更新设置
```

### 3.3 文档管理

```
文档 添加 <索引> --文件 <path.json>            从 JSON 文件添加文档
文档 添加 <索引> --json '[{"id":1,"t":"x"}]'   直接传入 JSON 添加文档
文档 更新 <索引> --文件 <path.json>            更新文档
文档 删除 <索引> <id>                          删除单个文档
文档 获取 <索引> <id>                          获取单个文档
```

`--文件` 与 `--json` 都接受单个对象或对象数组。

### 3.4 企业级搜索

```
搜索 <关键词> --索引 <名称>
搜索 <关键词> --索引 <名称> --筛选 "价格>5 AND 类别=电子"
搜索 <关键词> --索引 <名称> --排序 "价格:desc" --高亮 名称,简介
搜索 <关键词> --索引 <名称> --分面 类别,品牌 --限制 20
```

| 选项 | 对应 Meilisearch 参数 | 说明 |
|------|----------------------|------|
| `--筛选` | `filter` | 筛选表达式 |
| `--排序` | `sort` | 排序字段，逗号分隔多个 |
| `--高亮` | `attributesToHighlight` | 高亮字段，逗号分隔 |
| `--分面` | `facets` | 分面字段，逗号分隔 |
| `--限制` | `limit` | 返回条数上限（整数） |

### 3.5 同义词管理

```
同义词 设置 <索引> --词 '鞋,运动鞋;跑,慢跑'    设置同义词组（分号分隔组，逗号分隔组内词）
同义词 获取 <索引>                             获取当前同义词组
```

### 3.6 设置管理

```
设置 获取 <索引>                               读取索引全部设置
设置 更新 <索引> --筛选属性 a,b --排序属性 c   更新索引设置
```

---

## 4. 完整示例

```bash
# 1. 创建索引
python3 scripts/vux_plugin_manager.py run meilisearch "索引 创建 书籍 --主键 id"

# 2. 添加文档
python3 scripts/vux_plugin_manager.py run meilisearch \
  "文档 添加 书籍 --json '[{\"id\":1,\"标题\":\"三体\",\"价格\":30},{\"id\":2,\"标题\":\"球状闪电\",\"价格\":25}]'"

# 3. 设置可筛选/可排序属性
python3 scripts/vux_plugin_manager.py run meilisearch \
  "索引 设置 书籍 --筛选属性 价格 --排序属性 价格"

# 4. 全文搜索
python3 scripts/vux_plugin_manager.py run meilisearch \
  "搜索 三体 --索引 书籍"

# 5. 带筛选与排序的搜索
python3 scripts/vux_plugin_manager.py run meilisearch \
  "搜索 球 --索引 书籍 --筛选 '价格>20' --排序 '价格:desc' --高亮 标题"

# 6. 设置同义词
python3 scripts/vux_plugin_manager.py run meilisearch \
  "同义词 设置 书籍 --词 '科幻,硬科幻;小说,长篇'"

# 7. 删除文档
python3 scripts/vux_plugin_manager.py run meilisearch "文档 删除 书籍 2"
```

---

## 5. 常见问题

- **提示"缺少依赖 meilisearch"**：先执行 `pip install meilisearch`。
- **提示"未配置 host"**：设置 `VUS_MEILI_HOST` 环境变量或配置文件中的 `host`。
- **连接失败（MeilisearchCommunicationError）**：确认 Meilisearch 服务已启动且地址可达；若启用了 key，检查 `api_key` 是否正确。
- **`--分面数量` / `--限制` 报"必须是整数"**：确认传入的是纯数字。

---

## 6. 开发者

- 单元测试：`plugins/func/meilisearch/tests/test_meilisearch.py`
- 运行测试：

```bash
cd plugins/func/meilisearch && python3 -m pytest tests/ -v
```

- 命令解析：`run()` 将输入字符串按空格拆分，首 token 为子命令，`--xxx` 为选项（支持 `--xxx 值` 与 `--xxx` 两种形式，值自动去除包裹引号）。