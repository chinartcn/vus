#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
server.py —— HTML 高级排版设计器本地后端（纯 Python 3 标准库）

- 静态托管同目录前端文件（index.html / style.css / app.js / sample_design.json）
- 提供 POST /export：把设计 JSON 转成 .vus 源码，结果以 JSON 返回

用法：
    python3 server.py [--port 8000]
"""

import argparse
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

import vus_export

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".vus": "text/plain; charset=utf-8",
    ".png": "image/png",
    ".svg": "image/svg+xml",
}


def build_vus(design):
    """供其它模块复用的导出入口；异常由调用方捕获。"""
    return vus_export.export_to_vus(design)


class DesignerHandler(BaseHTTPRequestHandler):
    server_version = "VUSGuiDesigner/1.0"

    # ---- 静态文件 ----
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        if path in ("/", "/index.html"):
            self._serve_file("index.html", ".html")
            return
        # 阻止路径穿越
        name = os.path.basename(path)
        if not name and path.strip("/"):
            self._send_json(404, {"ok": False, "error": "not found"})
            return
        file_path = os.path.join(BASE_DIR, name)
        file_path = os.path.abspath(file_path)
        if os.path.dirname(file_path) != BASE_DIR:
            self._send_json(403, {"ok": False, "error": "forbidden"})
            return
        ext = os.path.splitext(name)[1]
        if os.path.isfile(file_path):
            self._serve_file(name, ext)
        else:
            self._send_json(404, {"ok": False, "error": "not found"})

    # ---- 导出 ----
    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/export":
            self._send_json(404, {"ok": False, "error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            raw = self.rfile.read(length).decode("utf-8")
            design = json.loads(raw)
            if not isinstance(design, dict):
                raise ValueError("请求体必须是 JSON 对象")
        except Exception as err:
            self._send_json(400, {"ok": False, "error": "请求体解析失败: %s" % err})
            return

        try:
            vus_src = vus_export.export_to_vus(design)
            self._send_json(200, {"ok": True, "vus": vus_src})
        except Exception as err:
            self._send_json(500, {"ok": False, "error": "导出失败: %s" % err})

    # ---- 内部工具 ----
    def _send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_file(self, name, ext):
        content_type = CONTENT_TYPES.get(ext, "application/octet-stream")
        try:
            with open(os.path.join(BASE_DIR, name), "rb") as f:
                data = f.read()
        except OSError:
            self._send_json(404, {"ok": False, "error": "not found"})
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        # 精简访问日志
        print("[%s] %s" % (self.address_string(), fmt % args))


def main():
    parser = argparse.ArgumentParser(description="HTML 高级排版设计器后端")
    parser.add_argument("--port", type=int, default=8000, help="监听端口（默认 8000）")
    args = parser.parse_args()

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), lambda *a, **kw: DesignerHandler(*a, **kw))
    print("listening on http://127.0.0.1:%d" % args.port)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止。")


if __name__ == "__main__":
    main()