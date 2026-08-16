"""
Meilisearch搜索插件 — VUS 功能插件 (.vux)

封装 Meilisearch 全文搜索引擎，提供中文子命令：
    状态 / 健康 / 统计                  — 服务健康与统计
    索引 列表 / 创建 / 删除 / 设置       — 索引管理
    文档 添加 / 更新 / 删除 / 获取       — 文档管理
    搜索 <关键词> --索引 <名> [...]     — 企业级全文搜索
    同义词 设置 / 获取                  — 同义词组管理

连接配置（优先级递减）：
    1. 环境变量 VUS_MEILI_HOST / VUS_MEILI_API_KEY
    2. 配置文件 ~/.vus/plugins/meilisearch/config.json（键：host / api_key）
    3. 默认 http://localhost:7700
"""

import json
import os
import sys
from pathlib import Path

# 将 scripts 目录加入路径以便导入 vux_plugin_entry
_scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..", "..", "scripts")
if _scripts_dir not in sys.path:
    sys.path.insert(0, _scripts_dir)

from vux_plugin_entry import VuxPlugin, VuxPluginAPI  # noqa: E402

# 默认配置
DEFAULT_HOST = "http://localhost:7700"
ENV_HOST = "VUS_MEILI_HOST"
ENV_API_KEY = "VUS_MEILI_API_KEY"
CONFIG_PATH = Path.home() / ".vus" / "plugins" / "meilisearch" / "config.json"


class MeilisearchPlugin(VuxPlugin):
    """Meilisearch 搜索插件 — 企业级全文搜索。"""

    def __init__(self):
        self._client = None
        self._api = None

    # ------------------------------------------------------------------
    # 生命周期
    # ------------------------------------------------------------------
    def init(self, api):
        """初始化插件：解析连接配置并建立 Meilisearch 客户端。"""
        self._api = api
        try:
            from meilisearch import Client
        except ImportError:
            print("  [Meilisearch] 缺少依赖 meilisearch，请先执行: pip install meilisearch")
            return -1

        host, api_key = self._load_config()
        if not host:
            print("  [Meilisearch] 未配置 host，无法连接。设置环境变量 VUS_MEILI_HOST 或配置文件。")
            return -1

        try:
            self._client = Client(host, api_key=api_key or None)
        except Exception as e:
            print(f"  [Meilisearch] 客户端初始化失败: {e}")
            return -1
        return 0

    def cleanup(self, api):
        """清理插件资源。"""
        self._client = None
        self._api = None

    # ------------------------------------------------------------------
    # 连接配置解析
    # ------------------------------------------------------------------
    def _load_config(self):
        """解析连接配置：环境变量 > 配置文件 > 默认值。"""
        host = os.environ.get(ENV_HOST, "")
        api_key = os.environ.get(ENV_API_KEY, "")

        if not host:
            cfg = self._read_config_file()
            if cfg:
                host = cfg.get("host", "")
                api_key = cfg.get("api_key", cfg.get("apiKey", api_key))

        if not host:
            host = DEFAULT_HOST

        return host, api_key

    def _read_config_file(self):
        """读取配置文件 ~/.vus/plugins/meilisearch/config.json。"""
        try:
            if CONFIG_PATH.is_file():
                with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                    return json.load(f)
        except Exception:
            pass
        return None

    @staticmethod
    def _get_index(client, name):
        """获取指定索引对象。"""
        return client.index(name)

    # ------------------------------------------------------------------
    # 命令解析
    # ------------------------------------------------------------------
    def _parse_command(self, line):
        """解析输入命令为 (args, options)。

        Args:
            line: 命令字符串，如 '搜索 hello --索引 书籍 --限制 10'
        Returns:
            (args_list, options_dict)
        """
        tokens = line.split()
        args = []
        options = {}
        i = 0
        while i < len(tokens):
            tok = tokens[i]
            if tok.startswith("--"):
                key = tok[2:]
                if i + 1 < len(tokens) and not tokens[i + 1].startswith("--"):
                    options[key] = self._strip_quotes(tokens[i + 1])
                    i += 2
                else:
                    options[key] = True
                    i += 1
            else:
                args.append(tok)
                i += 1
        return args, options

    @staticmethod
    def _strip_quotes(value):
        """去除值首尾包裹的单/双引号。"""
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            return value[1:-1]
        return value

    # ------------------------------------------------------------------
    # 主入口
    # ------------------------------------------------------------------
    def run(self, api, input_data):
        """执行插件主要功能。输入为中文命令字符串。"""
        if not self._client:
            return -1, "错误: 插件未初始化，请检查连接配置"

        line = (input_data or "").strip()
        if not line:
            return 0, self._usage()

        args, options = self._parse_command(line)
        if not args:
            return 0, self._usage()

        cmd = args[0]
        rest = args[1:]

        handlers = {
            "状态": self._cmd_health,
            "健康": self._cmd_health,
            "统计": self._cmd_stats,
            "索引": self._cmd_index,
            "文档": self._cmd_doc,
            "搜索": self._cmd_search,
            "同义词": self._cmd_synonym,
            "设置": self._cmd_settings,
        }

        handler = handlers.get(cmd)
        if not handler:
            return 0, f"错误: 未知子命令 '{cmd}'\n{self._usage()}"

        try:
            return handler(rest, options)
        except Exception as e:
            return 0, f"错误: 执行失败: {e}"

    # ------------------------------------------------------------------
    # 健康与统计
    # ------------------------------------------------------------------
    def _cmd_health(self, rest, options):
        """查询服务健康状态。"""
        status = self._client.health()
        return 0, json.dumps(status, ensure_ascii=False, default=str)

    def _cmd_stats(self, rest, options):
        """查询服务统计信息。"""
        stats = self._client.get_indexes_stats()
        # 简化输出：只保留数据库大小与索引概览
        summary = {
            "数据库大小": stats.get("databaseSize"),
            "索引数": stats.get("indexes") and len(stats.get("indexes", {})),
            "索引": list((stats.get("indexes") or {}).keys()),
        }
        return 0, json.dumps(summary, ensure_ascii=False, default=str)

    # ------------------------------------------------------------------
    # 索引管理
    # ------------------------------------------------------------------
    def _cmd_index(self, rest, options):
        """索引管理子命令：列表/创建/删除/设置。"""
        if not rest:
            return 0, "用法: 索引 列表|创建|删除|设置"

        sub = rest[0]
        name = rest[1] if len(rest) > 1 else None

        if sub == "列表":
            indexes = self._client.get_indexes()
            idx_list = [{"uid": i.get("uid"), "primaryKey": i.get("primaryKey")} for i in indexes]
            return 0, json.dumps(idx_list, ensure_ascii=False, default=str)

        if sub == "创建":
            if not name:
                return 0, "错误: 用法 索引 创建 <名称> [--主键 xxx]"
            primary_key = options.get("主键")
            task = self._client.create_index(name, primary_key=primary_key)
            return 0, f"索引创建任务已提交 uid={task.get('taskUid')}"

        if sub == "删除":
            if not name:
                return 0, "错误: 用法 索引 删除 <名称>"
            task = self._client.delete_index(name)
            return 0, f"索引删除任务已提交 uid={task.get('taskUid')}"

        if sub == "设置":
            return self._index_settings(name, options)

        return 0, f"错误: 未知索引子命令 '{sub}'"

    def _index_settings(self, name, options):
        """读取或更新索引设置。"""
        if not name:
            return 0, "错误: 用法 索引 设置 <名称> [--筛选属性 ...] [--分面数量 n]"
        index = self._get_index(self._client, name)
        if options:
            settings, err = self._build_settings_from_options(options)
            if err:
                return 0, err
            task = index.update_settings(settings)
            return 0, f"索引设置更新任务已提交 uid={task.get('taskUid')}"
        settings = index.get_settings()
        return 0, json.dumps(settings, ensure_ascii=False, default=str)

    # ------------------------------------------------------------------
    # 文档管理
    # ------------------------------------------------------------------
    def _cmd_doc(self, rest, options):
        """文档管理子命令：添加/更新/删除/获取。"""
        if not rest:
            return 0, "用法: 文档 添加|更新|删除|获取"

        sub = rest[0]
        index_name = rest[1] if len(rest) > 1 else None
        if not index_name:
            return 0, "错误: 缺少索引名称"

        index = self._get_index(self._client, index_name)

        if sub == "添加" or sub == "更新":
            docs = self._load_documents(options)
            if docs is None:
                return 0, "错误: 需通过 --文件 <path> 或 --json '<json>' 提供文档"
            primary_key = options.get("主键")
            if sub == "添加":
                task = index.add_documents(docs, primary_key=primary_key)
            else:
                task = index.update_documents(docs, primary_key=primary_key)
            return 0, f"文档{sub}任务已提交 uid={task.get('taskUid')} 条数={len(docs)}"

        if sub == "删除":
            doc_id = rest[2] if len(rest) > 2 else None
            if not doc_id:
                return 0, "错误: 用法 文档 删除 <索引> <id>"
            task = index.delete_document(doc_id)
            return 0, f"文档删除任务已提交 uid={task.get('taskUid')}"

        if sub == "获取":
            doc_id = rest[2] if len(rest) > 2 else None
            if not doc_id:
                return 0, "错误: 用法 文档 获取 <索引> <id>"
            doc = index.get_document(doc_id)
            return 0, json.dumps(doc, ensure_ascii=False, default=str)

        return 0, f"错误: 未知文档子命令 '{sub}'"

    def _load_documents(self, options):
        """从 --文件 或 --json 加载文档列表。"""
        if "文件" in options:
            path = options["文件"]
            try:
                with open(path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                return data if isinstance(data, list) else [data]
            except Exception as e:
                return None
        if "json" in options:
            try:
                data = json.loads(options["json"])
                return data if isinstance(data, list) else [data]
            except Exception as e:
                return None
        return None

    # ------------------------------------------------------------------
    # 企业级搜索
    # ------------------------------------------------------------------
    def _cmd_search(self, rest, options):
        """企业级全文搜索。"""
        if not rest:
            return 0, "用法: 搜索 <关键词> --索引 <名称> [--筛选] [--排序] [--高亮] [--分面] [--限制]"

        query = rest[0]
        index_name = options.get("索引")
        if not index_name:
            return 0, "错误: 必须通过 --索引 <名称> 指定搜索的索引"

        params = {}
        if "筛选" in options:
            params["filter"] = options["筛选"]
        if "排序" in options:
            # 支持多个排序字段，用逗号分隔
            params["sort"] = self._split_csv(options["排序"])
        if "高亮" in options:
            params["attributesToHighlight"] = self._split_csv(options["高亮"])
        if "分面" in options:
            params["facets"] = self._split_csv(options["分面"])
        if "限制" in options:
            try:
                params["limit"] = int(options["限制"])
            except ValueError:
                return 0, "错误: --限制 必须是整数"

        index = self._get_index(self._client, index_name)
        result = index.search(query, **params)

        hits = result.get("hits", [])
        summary = {
            "命中数": result.get("estimatedTotalHits"),
            "耗时_ms": result.get("processingTimeMs"),
            "限制": result.get("limit"),
            "结果": hits,
        }
        return 0, json.dumps(summary, ensure_ascii=False, default=str)

    # ------------------------------------------------------------------
    # 同义词管理
    # ------------------------------------------------------------------
    def _cmd_synonym(self, rest, options):
        """同义词管理子命令：设置/获取。"""
        if not rest:
            return 0, "用法: 同义词 设置|获取 <索引>"

        sub = rest[0]
        index_name = rest[1] if len(rest) > 1 else None
        if not index_name:
            return 0, "错误: 缺少索引名称"

        index = self._get_index(self._client, index_name)

        if sub == "设置":
            # --词 "鞋,运动鞋;跑步,慢跑"  分号分隔组，逗号分隔组内词
            groups_raw = options.get("词", "")
            if not groups_raw:
                return 0, "错误: 用法 同义词 设置 <索引> --词 '组1;组2'"
            synonyms = {}
            for group in groups_raw.split(";"):
                words = [w.strip() for w in group.split(",") if w.strip()]
                if len(words) >= 2:
                    synonyms[words[0]] = words[1:]
            task = index.update_synonyms(synonyms)
            return 0, f"同义词更新任务已提交 uid={task.get('taskUid')}"

        if sub == "获取":
            settings = index.get_settings()
            synonyms = settings.get("synonyms", {})
            return 0, json.dumps(synonyms, ensure_ascii=False, default=str)

        return 0, f"错误: 未知同义词子命令 '{sub}'"

    # ------------------------------------------------------------------
    # 设置管理
    # ------------------------------------------------------------------
    def _cmd_settings(self, rest, options):
        """设置管理子命令：获取/更新。"""
        if not rest:
            return 0, "用法: 设置 获取|更新 <索引>"

        sub = rest[0]
        index_name = rest[1] if len(rest) > 1 else None
        if not index_name:
            return 0, "错误: 缺少索引名称"

        index = self._get_index(self._client, index_name)

        if sub == "获取":
            settings = index.get_settings()
            return 0, json.dumps(settings, ensure_ascii=False, default=str)

        if sub == "更新":
            if not options:
                return 0, "错误: 用法 设置 更新 <索引> --筛选属性 ... --排序属性 ..."
            settings, err = self._build_settings_from_options(options)
            if err:
                return 0, err
            task = index.update_settings(settings)
            return 0, f"设置更新任务已提交 uid={task.get('taskUid')}"

        return 0, f"错误: 未知设置子命令 '{sub}'"

    # ------------------------------------------------------------------
    # 工具方法
    # ------------------------------------------------------------------
    @staticmethod
    def _build_settings_from_options(options):
        """从选项构建更新用的设置字典。

        支持 --筛选属性、--排序属性、--分面数量。
        Returns:
            (settings_dict, None) 成功；或 (None, error_msg) 参数非法。
        """
        settings = {}
        if "筛选属性" in options:
            settings["filterableAttributes"] = MeilisearchPlugin._split_csv(options["筛选属性"])
        if "排序属性" in options:
            settings["sortableAttributes"] = MeilisearchPlugin._split_csv(options["排序属性"])
        if "分面数量" in options:
            try:
                settings["pagination"] = {"maxTotalHits": int(options["分面数量"])}
            except ValueError:
                return None, "错误: --分面数量 必须是整数"
        return settings, None

    @staticmethod
    def _split_csv(value):
        """将逗号分隔的字符串拆为列表。"""
        return [v.strip() for v in value.split(",") if v.strip()]

    @staticmethod
    def _usage():
        """返回用法说明。"""
        return (
            "Meilisearch 搜索插件命令:\n"
            "  状态 / 健康 / 统计\n"
            "  索引 列表 | 创建 <名> [--主键 xxx] | 删除 <名> | 设置 <名> [--筛选属性 a,b] [--分面数量 n]\n"
            "  文档 添加 <索引> --文件 path|--json '[...]' | 更新 | 删除 <索引> <id> | 获取 <索引> <id>\n"
            "  搜索 <关键词> --索引 <名> [--筛选 'x'] [--排序 'a,b'] [--高亮 a,b] [--分面 a,b] [--限制 n]\n"
            "  同义词 设置 <索引> --词 '组1;组2' | 获取 <索引>\n"
            "  设置 获取|更新 <索引> [--筛选属性 a,b] [--排序属性 a,b]\n"
            "连接: 环境变量 VUS_MEILI_HOST/VUS_MEILI_API_KEY 或 ~/.vus/plugins/meilisearch/config.json"
        )