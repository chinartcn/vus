#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
server.py —— VUS 初期 IDE 后端（纯 Python 3 标准库）

- 静态托管前端（index.html / app/ / libs/）
- 路由：
    POST /export             设计 JSON → .vus（免 Token）
    POST /api/compile        代码编译检查（免 Token）
    POST /api/run            执行 vus / shell（需 Token，SSE 流式输出）
    GET  /api/search         文档检索（免 Token）
    GET  /api/fs/list|read   工程文件读取（免 Token）
    POST /api/fs/write       工程文件写入（需 Token）

只绑定 127.0.0.1。启动时生成随机 Token（或用 --token 指定）并打印到控制台；
带副作用接口需在请求头 X-VUS-Token 携带该 Token。

用法：
    python3 server.py [--port 8000] [--token 自定义]
"""

import argparse
import json
import os
import sys
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import api

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".vus": "text/plain; charset=utf-8",
    ".png": "image/png",
    ".svg": "image/svg+xml",
    ".map": "application/json",
}


# ---------- 路径安全（静态文件，允许子目录、阻止穿越） ----------

def _safe_static(rel):
    rel = rel.lstrip("/")
    target = os.path.realpath(os.path.join(BASE_DIR, rel))
    if target != BASE_DIR and not target.startswith(BASE_DIR.rstrip("/") + os.sep):
        return None
    return target


class IdeHandler(BaseHTTPRequestHandler):
    server_version = "VUSIde/1.0"

    # ---------- GET ----------
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path in ("/", "/index.html"):
            self._serve_file(os.path.join(BASE_DIR, "index.html"), ".html")
            return

        if path.startswith("/api/"):
            self._route_api_get(path, parsed)
            return

        target = _safe_static(path)
        if target and os.path.isfile(target):
            ext = os.path.splitext(target)[1]
            self._serve_file(target, ext)
        else:
            self._send_json(404, {"ok": False, "error": "not found"})

    def _route_api_get(self, path, parsed):
        qs = parse_qs(parsed.query)
        try:
            if path == "/api/search":
                q = (qs.get("q", [""])[0]).strip()
                engine = qs.get("engine", ["local"])[0]
                # 仅在设置里显式给出的 meili_url 才允许联调；否则回退 local
                req_meili = qs.get("meili_url", [""])[0]
                meili_url = req_meili if req_meili.startswith(("http://", "https://")) else None
                res = api.search_index(q, engine=engine, meili_url=meili_url)
                self._send_json(200, {"ok": True, **res})
                return
            if path == "/api/fs/list":
                rel = qs.get("path", ["."])[0]
                self._send_json(200, {"ok": True, "items": api.list_dir(BASE_DIR, rel)})
                return
            if path == "/api/fs/read":
                rel = qs.get("path", [""])[0]
                text = api.read_file(rel, BASE_DIR)
                self._send_json(200, {"ok": True, "text": text})
                return
        except Exception as err:
            self._send_json(403, {"ok": False, "error": str(err)})
            return
        self._send_json(404, {"ok": False, "error": "not found"})

    # ---------- POST ----------
    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/export":
            self._handle_export()
            return
        if path == "/api/compile":
            self._handle_compile()
            return
        if path == "/api/run":
            self._handle_run()
            return
        if path == "/api/fs/write":
            self._handle_fs_write()
            return
        self._send_json(404, {"ok": False, "error": "not found"})

    def _handle_export(self):
        try:
            design = self._read_json()
        except Exception as err:
            self._send_json(400, {"ok": False, "error": "请求体解析失败: %s" % err})
            return
        try:
            vus = api.export_design(design)
            self._send_json(200, {"ok": True, "vus": vus})
        except Exception as err:
            self._send_json(500, {"ok": False, "error": "导出失败: %s" % err})

    def _handle_compile(self):
        try:
            body = self._read_json()
            source = body.get("source", "")
        except Exception as err:
            self._send_json(400, {"ok": False, "error": "请求体解析失败: %s" % err})
            return
        try:
            res = api.compile_source(source)
            self._send_json(200, {"ok": True, **res})
        except Exception as err:
            self._send_json(500, {"ok": False, "error": "编译失败: %s" % err})

    def _handle_run(self):
        # r SSR：带副作用，必须校验 Token
        if not self._check_auth():
            self._send_json(401, {"ok": False, "error": "未授权：请在设置页填写后端 Token"})
            return
        try:
            body = self._read_json()
        except Exception as err:
            self._send_json(400, {"ok": False, "error": "请求体解析失败: %s" % err})
            return
        kind = body.get("kind", "shell")
        timeout = int(body.get("timeout", 15))
        max_output = int(body.get("max_output", 500))
        # 立即以 SSE 头开始，逐步写数据
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache, no-transform")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            for evt, payload in api.run_process(
                    kind, source=body.get("source"), command=body.get("command"),
                    timeout=timeout, max_output=max_output):
                line = json.dumps({"e": evt, "d": payload}, ensure_ascii=False)
                self.wfile.write(("data: %s\n\n" % line).encode("utf-8"))
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as err:
            traceback.print_exc()
            try:
                line = json.dumps({"e": "err", "d": "服务端异常：%s\n" % err}, ensure_ascii=False)
                self.wfile.write(("data: %s\n\n" % line).encode("utf-8"))
                self.wfile.flush()
            except Exception:
                pass

    def _handle_fs_write(self):
        if not self._check_auth():
            self._send_json(401, {"ok": False, "error": "未授权：请在设置页填写后端 Token"})
            return
        try:
            body = self._read_json()
            rel = body.get("path", "")
            text = body.get("text", "")
            api.write_file(rel, text, BASE_DIR)
            self._send_json(200, {"ok": True, "path": rel})
        except Exception as err:
            self._send_json(403, {"ok": False, "error": str(err)})

    # ---------- 工具 ----------
    def _check_auth(self):
        provided = self.headers.get("X-VUS-Token")
        return api.check_token(provided) if provided else False

    def _read_json(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8")
        data = json.loads(raw) if raw else {}
        return data

    def _send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_file(self, path, ext):
        content_type = CONTENT_TYPES.get(ext, "application/octet-stream")
        try:
            with open(path, "rb") as f:
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
    parser = argparse.ArgumentParser(description="VUS 初期 IDE 后端")
    parser.add_argument("--port", type=int, default=8000, help="监听端口（默认 8000）")
    parser.add_argument("--token", default=None, help="访问 Token（默认自动生成）")
    args = parser.parse_args()

    if args.token:
        token = args.token
    else:
        token = api.generate_token()
    api.set_token(token)

    try:
        httpd = ThreadingHTTPServer(("127.0.0.1", args.port),
                                    lambda *a, **kw: IdeHandler(*a, **kw))
    except OSError as err:
        print("启动失败：端口 %d 被占用或不可用（%s）\n请用 --port 更换端口。" % (args.port, err))
        sys.exit(1)

    print("=" * 52)
    print(" VUS IDE 已启动：  http://127.0.0.1:%d" % args.port)
    print(" 访问 Token：      %s" % token)
    print(" 说明：/api/run 与 文件写入 接口需要该 Token（设置页填写）。")
    print("=" * 52)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止。")


if __name__ == "__main__":
    main()