"""深度集成：生成 meilisearch 插件的配置文件，使其开箱即用。

写入 ~/.vus/plugins/meilisearch/config.json，内容为
{"host": "...", "api_key": "..."}，meilisearch 插件按优先级读取该配置。
"""

import json
from pathlib import Path

MEILISEARCH_CONFIG = Path("~/.vus/plugins/meilisearch/config.json").expanduser()


def write_meilisearch_config(host, api_key, config_path=None):
    """写入 meilisearch 插件配置文件。

    Args:
        host: Meilisearch 服务地址，如 http://localhost:7700
        api_key: master key
        config_path: 目标路径，缺省为 ~/.vus/plugins/meilisearch/config.json

    Returns:
        写入的绝对路径字符串。
    """
    path = Path(config_path) if config_path else MEILISEARCH_CONFIG
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {"host": host, "api_key": api_key}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    return str(path)