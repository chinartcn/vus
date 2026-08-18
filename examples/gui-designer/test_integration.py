#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VUS IDE 后端集成测试：启动 server 并验证各 API 端点。"""
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = __file__.rsplit("/", 1)[0]
PORT = 8123
TOKEN = "testtoken123"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def req(method, path, body=None, token=None):
    url = "http://127.0.0.1:%d%s" % (PORT, path)
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(url, data=data, method=method)
    r.add_header("Content-Type", "application/json")
    if token:
        r.add_header("X-VUS-Token", token)
    def _parse_raw(b):
        try:
            return json.loads(b.decode())
        except Exception:
            return b.decode()
    try:
        with urllib.request.urlopen(r, timeout=30) as resp:
            return resp.status, _parse_raw(resp.read())
    except urllib.error.HTTPError as e:
        return e.code, _parse_raw(e.read())


def main():
    port = free_port()
    global PORT
    PORT = port
    proc = subprocess.Popen(
        [sys.executable, "server.py", "--port", str(port), "--token", TOKEN],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        cwd=BASE, text=True, bufsize=1)
    time.sleep(1.2)
    passed, failed = [], []

    def check(name, cond, extra=""):
        if cond:
            passed.append(name)
            print("[PASS] " + name + ("  (" + extra + ")" if extra else ""))
        else:
            failed.append(name)
            print("[FAIL] " + name + ("  (" + extra + ")" if extra else ""))

    # 1. 静态资源
    code, _ = req("GET", "/")
    check("GET / 返回首页", code == 200)
    code, _ = req("GET", "/app/main.js")
    check("GET /app/main.js 可访问", code == 200)
    code, _ = req("GET", "/libs/vue.global.js")
    check("GET /libs/vue.global.js 可访问", code == 200)
    code, _ = req("GET", "/libs/cm/codemirror.min.js")
    check("GET /libs/cm/codemirror.min.js 可访问", code == 200)

    # 2. 导出
    design = {
        "name": "测试界面", "width": 320, "height": 240,
        "theme": {"bg": 0xFFFFFF, "border": 0x888888, "highlight": 0x0055AA,
                  "fg": 0x333333, "text": 0},
        "radius": 8,
        "controls": [
            {"type": "label", "name": "t1", "x": 10, "y": 8, "text": "你好", "color": 0, "size": "12"},
            {"type": "button", "name": "b1", "x": 20, "y": 40, "w": 100, "h": 30, "text": "确定"},
            {"type": "slider", "name": "s1", "x": 20, "y": 90, "w": 160, "value": 50, "min": 0, "max": 100},
            {"type": "switch", "name": "sw1", "x": 20, "y": 130, "on": True},
        ],
    }
    code, r = req("POST", "/export", design)
    vus = r.get("vus", "")
    check("POST /export 成功", code == 200 and r.get("ok"))
    check("导出含图形_初始化", "图形_初始化(320, 240," in vus)
    check("导出含图形_按钮", "图形_按钮(" in vus)
    check("导出含标记注释", "# 控件：b1" in vus)

    # 3. 编译检查（接口可调用：返回结构正确；是否通过取决于 vus 编译器是否安装）
    code, r = req("POST", "/api/compile", {"source": vus})
    check("POST /api/compile 接口可调用", code == 200 and isinstance(r.get("errors"), list))

    # 4. 文档检索（本地）
    q = urllib.parse.quote("图形_初始化")
    code, r = req("GET", "/api/search?q=" + q)
    check("GET /api/search q=图形_初始化", code == 200 and r.get("ok")
          and r.get("results") and r["results"][0]["name"] == "图形_初始化")

    # 5. 文件读写
    code, r = req("GET", "/api/fs/list?path=.")
    check("GET /api/fs/list", code == 200 and r.get("ok") and len(r.get("items", [])) > 0)
    code, r = req("GET", "/api/fs/read?path=README.md")
    check("GET /api/fs/read README", code == 200 and r.get("ok") and isinstance(r.get("text"), str))
    # 写文件需要 Token
    code, r = req("POST", "/api/fs/write", {"path": "tmptest.txt", "text": "hello"})
    check("fs/write 无 Token 被拒", code == 401)
    code, r = req("POST", "/api/fs/write", {"path": "tmptest.txt", "text": "hello"}, TOKEN)
    check("fs/write 带 Token 成功", code == 200 and r.get("ok"))
    code, r = req("GET", "/api/fs/read?path=tmptest.txt")
    check("fs/read 回读写入", code == 200 and r.get("text") == "hello")
    # 写路径越界
    code, r = req("POST", "/api/fs/write", {"path": "../att.txt", "text": "x"}, TOKEN)
    check("fs/write 路径越界被拒", code == 403)
    import os
    os.remove(os.path.join(BASE, "tmptest.txt"))

    # 6. 执行 shell（需 Token）
    code, r = req("POST", "/api/run", {"kind": "shell", "command": "echo hi"})
    check("run 无 Token 被拒", code == 401)
    # SSE 执行
    url = "http://127.0.0.1:%d/api/run" % PORT
    body = json.dumps({"kind": "shell", "command": "echo hi"}).encode()
    rq = urllib.request.Request(url, data=body, method="POST")
    rq.add_header("Content-Type", "application/json")
    rq.add_header("X-VUS-Token", TOKEN)
    with urllib.request.urlopen(rq, timeout=30) as resp:
        ct = resp.headers.get("Content-Type", "")
        sse = resp.read().decode()
    has_exit = '"e":"exit"' in sse or '"e": "exit"' in sse
    check("run SSE content-type", "event-stream" in ct)
    check("run SSE 含退出事件", has_exit)

    proc.terminate()
    proc.wait(timeout=5)

    print("\n===== 结果：%d 通过，%d 失败 =====" % (len(passed), len(failed)))
    if failed:
        sys.exit(1)
    print("全部通过")


if __name__ == "__main__":
    main()