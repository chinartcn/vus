"""
Meilisearch 搜索插件单元测试

使用 unittest.mock 隔离 meilisearch SDK，不依赖真实服务。
覆盖：
    - 命令解析（_parse_command 的选项解析）
    - 各子命令路由与参数传递（健康/索引/文档/搜索/同义词/设置）
    - 缺参、未知子命令、非法值的错误处理
    - 连接配置优先级（环境变量 > 配置文件 > 默认值）
"""

import json
import os
import sys
import unittest
from pathlib import Path
from unittest import mock

# 将插件目录加入路径
_PLUGIN_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PLUGIN_DIR))

import __init__ as plugin_mod  # noqa: E402


def make_plugin():
    """构造插件实例但不初始化客户端。"""
    return plugin_mod.MeilisearchPlugin()


class TestParseCommand(unittest.TestCase):
    """测试命令解析器。"""

    def setUp(self):
        self.plugin = make_plugin()

    def test_plain_args(self):
        args, opts = self.plugin._parse_command("搜索 hello")
        self.assertEqual(args, ["搜索", "hello"])
        self.assertEqual(opts, {})

    def test_options_parsed(self):
        args, opts = self.plugin._parse_command(
            "搜索 hello --索引 books --限制 10 --高亮 标题"
        )
        self.assertEqual(args, ["搜索", "hello"])
        self.assertEqual(opts, {"索引": "books", "限制": "10", "高亮": "标题"})

    def test_flag_without_value(self):
        args, opts = self.plugin._parse_command("索引 设置 books --主键")
        self.assertEqual(opts, {"主键": True})

    def test_empty(self):
        args, opts = self.plugin._parse_command("")
        self.assertEqual(args, [])
        self.assertEqual(opts, {})


class TestUnknownCommand(unittest.TestCase):
    """测试未知子命令。"""

    def test_unknown_command(self):
        plugin = make_plugin()
        plugin._client = mock.MagicMock()
        code, out = plugin.run(None, "乱七八糟 命令")
        self.assertEqual(code, 0)
        self.assertIn("未知子命令", out)

    def test_empty_input_returns_usage(self):
        plugin = make_plugin()
        plugin._client = mock.MagicMock()
        code, out = plugin.run(None, "")
        self.assertEqual(code, 0)
        self.assertIn("Meilisearch", out)


class TestHealthCommand(unittest.TestCase):
    """测试健康/统计命令。"""

    def test_health(self):
        plugin = make_plugin()
        client = mock.MagicMock()
        client.health.return_value = {"status": "available"}
        plugin._client = client
        code, out = plugin.run(None, "健康")
        self.assertEqual(code, 0)
        self.assertIn("available", out)

    def test_stats(self):
        plugin = make_plugin()
        client = mock.MagicMock()
        client.get_all_stats.return_value = {
            "databaseSize": 1024,
            "indexes": {"books": {"numberOfDocuments": 5}},
        }
        plugin._client = client
        code, out = plugin.run(None, "统计")
        self.assertEqual(code, 0)
        data = json.loads(out)
        self.assertEqual(data["索引数"], 1)
        self.assertEqual(data["索引"], ["books"])


class TestIndexCommand(unittest.TestCase):
    """测试索引管理。"""

    def setUp(self):
        self.plugin = make_plugin()
        self.client = mock.MagicMock()
        self.plugin._client = self.client

    def test_create_index(self):
        self.client.create_index.return_value = {"taskUid": 1}
        code, out = self.plugin.run(None, "索引 创建 books --主键 id")
        self.assertEqual(code, 0)
        self.client.create_index.assert_called_once_with(
            "books", options={"primaryKey": "id"})
        self.assertIn("uid=1", out)

    def test_create_index_no_primary(self):
        self.client.create_index.return_value = {"taskUid": 1}
        code, out = self.plugin.run(None, "索引 创建 books")
        self.assertEqual(code, 0)
        self.client.create_index.assert_called_once_with("books", options=None)

    def test_create_index_missing_name(self):
        code, out = self.plugin.run(None, "索引 创建")
        self.assertEqual(code, 0)
        self.assertIn("用法", out)

    def test_delete_index(self):
        self.client.delete_index.return_value = {"taskUid": 2}
        code, out = self.plugin.run(None, "索引 删除 books")
        self.client.delete_index.assert_called_once_with("books")
        self.assertIn("uid=2", out)

    def test_list_index(self):
        self.client.get_indexes.return_value = {
            "results": [type("Idx", (), {"uid": "books", "primary_key": "id"})()],
            "total": 1,
        }
        code, out = self.plugin.run(None, "索引 列表")
        data = json.loads(out)
        self.assertEqual(data[0]["uid"], "books")

    def test_list_index_dict_items(self):
        # 兼容旧版 SDK：返回 list[dict]
        self.client.get_indexes.return_value = [
            {"uid": "books", "primaryKey": "id"},
        ]
        code, out = self.plugin.run(None, "索引 列表")
        data = json.loads(out)
        self.assertEqual(data[0]["uid"], "books")

    def test_get_settings(self):
        index = mock.MagicMock()
        index.get_settings.return_value = {"synonyms": {}}
        self.client.index.return_value = index
        code, out = self.plugin.run(None, "索引 设置 books")
        data = json.loads(out)
        self.assertIn("synonyms", data)

    def test_unknown_index_subcommand(self):
        code, out = self.plugin.run(None, "索引 轰炸 书籍")
        self.assertIn("未知索引子命令", out)

    def test_create_index_invalid_uid(self):
        # 中文索引名非法，应直接返回提示而非调用 SDK
        self.plugin.run(None, "索引 创建 books --主键 id")
        self.client.create_index.assert_called_once()  # 上面的调用已执行
        self.client.reset_mock()
        code, out = self.plugin.run(None, "索引 创建 中文名")
        self.assertEqual(code, 0)
        self.assertIn("非法", out)
        self.client.create_index.assert_not_called()


class TestDocCommand(unittest.TestCase):
    """测试文档管理。"""

    def setUp(self):
        self.plugin = make_plugin()
        self.client = mock.MagicMock()
        self.index = mock.MagicMock()
        self.client.index.return_value = self.index
        self.plugin._client = self.client

    def test_add_documents_from_json(self):
        self.index.add_documents.return_value = {"taskUid": 3}
        code, out = self.plugin.run(
            None, "文档 添加 books --json '[{\"id\":1,\"t\":\"x\"}]'"
        )
        self.assertEqual(code, 0)
        self.index.add_documents.assert_called_once()
        self.assertIn("uid=3", out)
        self.assertIn("条数=1", out)

    def test_add_documents_missing_data(self):
        code, out = self.plugin.run(None, "文档 添加 books")
        self.assertIn("--文件", out)

    def test_add_documents_from_file(self):
        import tempfile
        docs_path = Path(tempfile.gettempdir()) / "meili_test_docs.json"
        docs_path.write_text(json.dumps([{"id": 1, "t": "x"}]), encoding="utf-8")
        try:
            self.index.add_documents.return_value = {"taskUid": 4}
            code, out = self.plugin.run(
                None, f"文档 添加 books --文件 {docs_path}"
            )
            self.assertEqual(code, 0)
            self.assertIn("uid=4", out)
        finally:
            docs_path.unlink(missing_ok=True)

    def test_delete_document(self):
        self.index.delete_document.return_value = {"taskUid": 5}
        code, out = self.plugin.run(None, "文档 删除 books 1")
        self.index.delete_document.assert_called_once_with("1")
        self.assertIn("uid=5", out)

    def test_get_document(self):
        self.index.get_document.return_value = {"id": 1, "t": "x"}
        code, out = self.plugin.run(None, "文档 获取 books 1")
        self.index.get_document.assert_called_once_with("1")
        data = json.loads(out)
        self.assertEqual(data["id"], 1)

    def test_get_document_model_object(self):
        # 兼容新版 SDK：get_document 返回 Document 模型对象
        class _Doc:
            def model_dump(self):
                return {"id": 2, "t": "y"}
        self.index.get_document.return_value = _Doc()
        code, out = self.plugin.run(None, "文档 获取 books 2")
        data = json.loads(out)
        self.assertEqual(data["id"], 2)
        self.assertEqual(data["t"], "y")

    def test_get_document_attribute_object(self):
        # 兼容新版 SDK：Document 字段为实例属性，无 dump 方法
        class _AttrDoc:
            def __init__(self):
                self.id = 3
                self.书名 = "三体"
        self.index.get_document.return_value = _AttrDoc()
        code, out = self.plugin.run(None, "文档 获取 books 3")
        data = json.loads(out)
        self.assertEqual(data["id"], 3)
        self.assertEqual(data["书名"], "三体")


class TestSearchCommand(unittest.TestCase):
    """测试企业级搜索。"""

    def setUp(self):
        self.plugin = make_plugin()
        self.client = mock.MagicMock()
        self.index = mock.MagicMock()
        self.client.index.return_value = self.index
        self.plugin._client = self.client

    def test_search_basic(self):
        self.index.search.return_value = {
            "estimatedTotalHits": 1,
            "processingTimeMs": 5,
            "limit": 20,
            "hits": [{"id": 1}],
        }
        code, out = self.plugin.run(None, "搜索 苹果 --索引 books")
        self.assertEqual(code, 0)
        self.index.search.assert_called_once_with("苹果")

    def test_search_with_options(self):
        self.index.search.return_value = {"hits": []}
        code, out = self.plugin.run(
            None,
            "搜索 苹果 --索引 books --筛选 '价格>5' --排序 '价格:desc' "
            "--高亮 名称 --分面 类别 --限制 10",
        )
        self.assertEqual(code, 0)
        _, kwargs = self.index.search.call_args
        self.assertEqual(kwargs["filter"], "价格>5")
        self.assertEqual(kwargs["sort"], ["价格:desc"])
        self.assertEqual(kwargs["attributesToHighlight"], ["名称"])
        self.assertEqual(kwargs["facets"], ["类别"])
        self.assertEqual(kwargs["limit"], 10)

    def test_search_missing_index(self):
        code, out = self.plugin.run(None, "搜索 苹果")
        self.assertIn("--索引", out)

    def test_search_invalid_limit(self):
        self.index.search.side_effect = None
        code, out = self.plugin.run(None, "搜索 苹果 --索引 books --限制 abc")
        self.assertIn("整数", out)


class TestSynonymCommand(unittest.TestCase):
    """测试同义词管理。"""

    def setUp(self):
        self.plugin = make_plugin()
        self.client = mock.MagicMock()
        self.index = mock.MagicMock()
        self.client.index.return_value = self.index
        self.plugin._client = self.client

    def test_set_synonyms(self):
        self.index.update_synonyms.return_value = {"taskUid": 6}
        code, out = self.plugin.run(
            None, "同义词 设置 books --词 '鞋,运动鞋;跑,慢跑'"
        )
        self.assertEqual(code, 0)
        call_args = self.index.update_synonyms.call_args
        self.assertEqual(call_args.args[0]["鞋"], ["运动鞋"])
        self.assertEqual(call_args.args[0]["跑"], ["慢跑"])

    def test_get_synonyms(self):
        self.index.get_settings.return_value = {"synonyms": {"鞋": ["运动鞋"]}}
        code, out = self.plugin.run(None, "同义词 获取 books")
        data = json.loads(out)
        self.assertEqual(data["鞋"], ["运动鞋"])


class TestSettingsCommand(unittest.TestCase):
    """测试设置管理。"""

    def setUp(self):
        self.plugin = make_plugin()
        self.client = mock.MagicMock()
        self.index = mock.MagicMock()
        self.client.index.return_value = self.index
        self.plugin._client = self.client

    def test_get_settings(self):
        self.index.get_settings.return_value = {"filterableAttributes": []}
        code, out = self.plugin.run(None, "设置 获取 books")
        data = json.loads(out)
        self.assertIn("filterableAttributes", data)

    def test_update_settings(self):
        self.index.update_settings.return_value = {"taskUid": 7}
        code, out = self.plugin.run(None, "设置 更新 books --筛选属性 a,b")
        call_args = self.index.update_settings.call_args
        self.assertEqual(call_args.args[0]["filterableAttributes"], ["a", "b"])


class TestConnectionConfig(unittest.TestCase):
    """测试连接配置优先级。"""

    def setUp(self):
        self.plugin = make_plugin()
        self._env = {}
        for k in (plugin_mod.ENV_HOST, plugin_mod.ENV_API_KEY):
            if k in os.environ:
                self._env[k] = os.environ.pop(k)

    def tearDown(self):
        for k, v in self._env.items():
            os.environ[k] = v

    @mock.patch("os.environ.get")
    def test_env_priority(self, mock_get):
        mock_get.side_effect = lambda k, d="": {
            "VUS_MEILI_HOST": "http://env:7700",
            "VUS_MEILI_API_KEY": "envkey",
        }.get(k, d)
        host, api_key = self.plugin._load_config()
        self.assertEqual(host, "http://env:7700")
        self.assertEqual(api_key, "envkey")

    def test_config_file_fallback(self):
        # 直接 patch _read_config_file 验证配置文件兜底
        with mock.patch.object(self.plugin, "_read_config_file",
                               return_value={"host": "http://cfg:7700", "api_key": "cfgkey"}):
            with mock.patch.dict(os.environ, {}, clear=True):
                host, api_key = self.plugin._load_config()
        self.assertEqual(host, "http://cfg:7700")
        self.assertEqual(api_key, "cfgkey")

    @mock.patch.object(plugin_mod, "CONFIG_PATH")
    def test_default_host(self, mock_path):
        mock_path.is_file.return_value = False
        with mock.patch.dict(os.environ, {}, clear=True):
            host, api_key = self.plugin._load_config()
        self.assertEqual(host, plugin_mod.DEFAULT_HOST)


if __name__ == "__main__":
    unittest.main()