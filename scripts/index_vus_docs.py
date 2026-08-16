#!/usr/bin/env python3
"""将 VUS 仓库的 Markdown 文档拆分为章节并灌入 Meilisearch 索引。

用法:
    python3 scripts/index_vus_docs.py --host <HOST> --key <MASTER_KEY> [--index vus-docs]

说明:
    - 解析仓库根目录下的 *.md 文档
    - 将每个文档按标题(##/###)拆分为多个章节文档
    - 每个章节包含: id / title / section / url / content / 所属文件
    - 设置可搜索 / 可显示属性后灌入 Meilisearch
"""
import argparse
import hashlib
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_INDEX = "vus-docs"

# 需要索引的文档（相对仓库根目录）
DOC_PATHS = [
    "README.md",
] + sorted(str(p.relative_to(REPO_ROOT)) for p in (REPO_ROOT / "docs").rglob("*.md"))


def strip_md(text):
    """去除 Markdown 标记，保留纯文本。"""
    text = re.sub(r"```.*?```", " ", text, flags=re.S)  # 代码块
    text = re.sub(r"`([^`]*)`", r"\1", text)             # 行内代码
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text)      # 图片
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)  # 链接
    text = re.sub(r"^#{1,6}\s*", "", text, flags=re.M)    # 标题标记
    text = re.sub(r"^\s*[-*+]\s+", " ", text, flags=re.M) # 列表
    text = re.sub(r"^\s*\d+\.\s+", " ", text, flags=re.M) # 有序列表
    text = re.sub(r"\|", " ", text)                        # 表格分隔
    text = re.sub(r"[-_*~#>]", " ", text)
    text = re.sub(r"\s+", " ", text).strip()
    return text


def split_sections(content, path):
    """按标题拆分 markdown 为章节列表。"""
    lines = content.splitlines()
    sections = []
    cur_title = None
    cur_buf = []
    cur_level = 0

    def flush():
        nonlocal cur_buf
        if cur_title is None:
            return
        text = strip_md("\n".join(cur_buf))
        if not text:
            return
        sections.append({"title": cur_title, "content": text})
        cur_buf = []

    for line in lines:
        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m:
            flush()
            level = len(m.group(1))
            title = m.group(2).strip()
            if level == 1:
                cur_title = title
                cur_level = level
            else:
                # 非一级标题作为独立章节，标题用 "父标题 - 子标题"
                cur_title = title
                cur_level = level
        else:
            if cur_title is not None:
                cur_buf.append(line)
    flush()
    return sections


def build_docs():
    """收集所有文档章节。"""
    docs = []
    for rel in DOC_PATHS:
        p = REPO_ROOT / rel
        if not p.exists():
            continue
        content = p.read_text(encoding="utf-8")
        sections = split_sections(content, rel)
        if not sections:
            # 无标题文档，整篇作为一节
            text = strip_md(content)
            if text:
                sections = [{"title": p.stem, "content": text}]
        for i, sec in enumerate(sections):
            doc_id = hashlib.md5(f"{rel}#{i}#{sec['title']}".encode()).hexdigest()[:16]
            docs.append({
                "id": doc_id,
                "title": sec["title"] or p.stem,
                "category": rel.split("/")[1] if rel.startswith("docs/") and len(rel.split("/")) > 1 else ("根目录" if rel == "README.md" else "docs"),
                "url": f"https://gitee.com/rtccn_mc/vus/blob/master/{rel}",
                "file": rel,
                "content": sec["content"],
            })
    return docs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--key", required=True, help="master key")
    ap.add_argument("--index", default=DEFAULT_INDEX)
    args = ap.parse_args()

    from meilisearch import Client
    client = Client(args.host, api_key=args.key)

    docs = build_docs()
    print(f"共构建 {len(docs)} 个文档章节")

    # 创建索引
    try:
        client.delete_index(args.index)
    except Exception:
        pass
    client.create_index(args.index, options={"primaryKey": "id"})

    import time
    time.sleep(2)
    index = client.index(args.index)

    # 设置可搜索 / 可显示属性
    index.update_filterable_attributes(["category", "file"])
    index.update_searchable_attributes(["title", "content", "category"])
    index.update_displayed_attributes(["id", "title", "category", "url", "file", "content"])

    # 灌入文档
    task = index.add_documents(docs)
    print(f"已提交文档灌入任务 uid={getattr(task, 'task_uid', task.get('taskUid') if isinstance(task, dict) else None)}")

    time.sleep(3)
    stats = client.get_index_stats(args.index) if hasattr(client, "get_index_stats") else None
    if stats is None:
        stats = client.index(args.index).get_stats()
    if hasattr(stats, "number_of_documents"):
        count = stats.number_of_documents
    elif isinstance(stats, dict):
        count = stats.get("numberOfDocuments")
    else:
        count = getattr(stats, "numberOfDocuments", "?")
    print(f"索引 \"{args.index}\" 文档数: {count}")
    print("完成。可在前端用 search key 搜索该索引。")


if __name__ == "__main__":
    main()