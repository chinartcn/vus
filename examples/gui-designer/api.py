#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
api.py —— VUS IDE 后端业务逻辑（纯 Python 3 标准库）

含所有 API 的纯逻辑实现，与 HTTP 层（server.py）解耦，便于单元测试：
- export_design    : 设计 JSON → .vus（委托 vus_export）
- compile_source   : 语法/编译检查（调用 vus build --c-only）
- run_process      : 执行 vus 脚本或 shell 命令，流式产出（生成器）
- list_dir/read/write：工程文件读写（含路径越界防护）
- search_index     : 文档检索（本地兜底 / 可选 Meilisearch）
- check_token      : Token 校验

安全边界：只应绑定 127.0.0.1；带副作用的 run/fs-写需要 Token。
"""

import hashlib
import hmac
import json
import os
import re
import select
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
# VUS 编译器可执行文件，可通过环境变量 VUS_BIN 覆盖
DEFAULT_VUS_BIN = os.environ.get("VUS_BIN", "vus")

# 由 server.py 设置
TOKEN = None

# 返回分隔符常量
EVT_OUT = "out"
EVT_ERR = "err"
EVT_EXIT = "exit"


def set_token(token):
    """设置访问 Token；None 表示未启用 Token 校验（仅本地调试）。"""
    global TOKEN
    TOKEN = token


def check_token(provided):
    """恒定时间比较请求提供的 Token。"""
    if TOKEN is None:
        return True
    if not provided:
        return False
    return hmac.compare_digest(str(provided), TOKEN)


def generate_token():
    """生成一个随机 Token。"""
    return hashlib.sha256(os.urandom(24)).hexdigest()[:32]


# ============ 导出 ============

def export_design(design):
    import vus_export
    return vus_export.export_to_vus(design)


# ============ 编译检查 ============

def compile_source(source, vus_bin=DEFAULT_VUS_BIN):
    """对源码做编译检查。返回 {ok, errors:[{line,col,msg}]}。"""
    if not isinstance(source, str) or not source.strip():
        return {"ok": True, "errors": []}
    tmp = tempfile.NamedTemporaryFile("w", suffix=".vus",
                                      delete=False, encoding="utf-8")
    try:
        tmp.write(source)
        tmp.close()
        try:
            proc = subprocess.run([vus_bin, "build", "--c-only", tmp.name],
                                  capture_output=True, text=True, timeout=60)
        except FileNotFoundError:
            return {"ok": False, "errors": [{"line": 0, "col": 0,
                                             "msg": "找不到 VUS 编译器（vus），请检查 PATH 或设置 VUS_BIN"}]}
        except subprocess.TimeoutExpired:
            return {"ok": False, "errors": [{"line": 0, "col": 0, "msg": "编译超时"}]}
    finally:
        try:
            os.remove(tmp.name)
        except OSError:
            pass

    combined = (proc.stderr or "") + "\n" + (proc.stdout or "")
    errors = _parse_compile_errors(combined)
    return {"ok": proc.returncode == 0, "errors": errors}


def _parse_compile_errors(text):
    """从编译输出解析错误行。优先匹配 x.c:LINE:COL: error: MSG。"""
    out = []
    pat = re.compile(r"(?:\.c|\.vus):(\d+):(\d+):\s*(?:error|fatal error):?\s*(.*)", re.IGNORECASE)
    for m in pat.finditer(text):
        out.append({"line": int(m.group(1)), "col": int(m.group(2)),
                    "msg": m.group(3).strip()})
    # 顶层 'error: msg' 兜底
    for line in text.splitlines():
        m = re.match(r"\s*(?:error|fatal error):\s*(.*)", line, re.IGNORECASE)
        if m and not any(e["msg"].startswith(m.group(1).strip()) for e in out):
            out.append({"line": 0, "col": 0, "msg": m.group(1).strip()})
    seen = set()
    res = []
    for e in out:
        k = (e["line"], e["msg"])
        if k in seen:
            continue
        seen.add(k)
        res.append(e)
    return res


# ============ 执行器（vus / shell）============

def _build_cmd(kind, source, command, vus_bin):
    if kind == "shell":
        return command, True
    if kind == "vus":
        if not source:
            return None, True
        tmp = tempfile.NamedTemporaryFile("w", suffix=".vus",
                                          delete=False, encoding="utf-8")
        tmp.write(source)
        tmp.close()
        return [vus_bin, "run", tmp.name], False
    return None, False


def run_process(kind, source=None, command=None, timeout=15, max_output=500,
                vus_bin=DEFAULT_VUS_BIN):
    """执行 vus 或 shell。生成器产出 (EVT_OUT|EVT_ERR|EVT_EXIT, payload)。"""
    return _run(kind, source, command, timeout, max_output, vus_bin)


def _run(kind, source, command, timeout, max_output, vus_bin):
    cmd, use_shell = _build_cmd(kind, source, command, vus_bin)
    if cmd is None:
        yield (EVT_ERR, "kind 仅支持 vus | shell（source/command 为空）。\n")
        yield (EVT_EXIT, -1)
        return
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                bufsize=1, shell=use_shell)
    except Exception as err:  # 找不到可执行文件等
        yield (EVT_ERR, "启动失败：%s\n" % err)
        yield (EVT_EXIT, -1)
        return

    start = time.time()
    count = 0
    line = ""
    try:
        while True:
            if proc.stdout is None:
                break
            rlist, _, _ = select.select([proc.stdout], [], [], 0.4)
            if rlist:
                line = proc.stdout.readline()
                if not line:
                    break
                count += 1
                if count > max_output:
                    yield (EVT_OUT, "[输出已达上限 %d 行，已截断]\n" % max_output)
                    _kill(proc)
                    break
                yield (EVT_OUT, line)
            else:
                if time.time() - start > timeout:
                    yield (EVT_ERR, "[已超时（%ss），进程被终止]\n" % timeout)
                    _kill(proc)
                    break
    except Exception as err:
        yield (EVT_ERR, "读取输出异常：%s\n" % err)
        _kill(proc)

    proc.wait()
    yield (EVT_EXIT, proc.returncode)


def _kill(proc):
    try:
        proc.kill()
    except Exception:
        pass


# ============ 文件边界 ============

def _real(base, rel):
    base = os.path.realpath(base)
    target = os.path.realpath(os.path.join(base, rel))
    if target != base and not target.startswith(base + os.sep):
        raise PermissionError("路径越界，禁止访问工程目录之外")
    return target


def list_dir(root=BASE_DIR, rel="."):
    """列出工程目录；返回 {path,type,name}[]。"""
    d = _real(root, rel)
    if not os.path.isdir(d):
        return []
    out = []
    for name in sorted(os.listdir(d)):
        if name.startswith("."):
            continue
        p = os.path.join(d, name)
        relp = os.path.relpath(os.path.join(rel, name), ".")
        relp = os.path.normpath(relp).replace("\\", "/")
        out.append({"path": relp, "type": "dir" if os.path.isdir(p) else "file",
                    "name": name})
    return out


def read_file(rel, root=BASE_DIR):
    p = _real(root, rel)
    if not os.path.isfile(p):
        raise FileNotFoundError("文件不存在")
    with open(p, "r", encoding="utf-8") as f:
        return f.read()


def write_file(rel, text, root=BASE_DIR):
    p = _real(root, rel)
    d = os.path.dirname(p)
    try:
        if d and not os.path.exists(d):
            os.makedirs(d, exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
    except FileNotFoundError:
        raise PermissionError("路径越界，禁止写入工程目录之外")


# ============ 本地文档索引 ============

_INDEX_CACHE = None
_INDEX_MTIME = None


def _load_index():
    global _INDEX_CACHE, _INDEX_MTIME
    path = os.path.join(BASE_DIR, "docs_index.json")
    try:
        mtime = os.path.getmtime(path)
    except OSError:
        return []
    if _INDEX_CACHE is not None and _INDEX_MTIME == mtime:
        return _INDEX_CACHE
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        data = []
    _INDEX_CACHE = data
    _INDEX_MTIME = mtime
    return data


def _score(item, q):
    q = q.lower()
    name = (item.get("name") or "").lower()
    sig = (item.get("signature") or "").lower()
    doc = (item.get("doc") or "").lower()
    ex = (item.get("example") or "").lower()
    cat = (item.get("category") or "").lower()
    s = 0
    if q in name:
        s += 60
    if name.startswith(q):
        s += 20
    if q in sig:
        s += 12
    if q in cat:
        s += 6
    if q in doc:
        s += 3
    if q in ex:
        s += 1
    return s


def _search_local(q, limit=30):
    if not q:
        return []
    scored = []
    for item in _load_index():
        s = _score(item, q)
        if s > 0:
            scored.append((s, item))
    scored.sort(key=lambda t: t[0], reverse=True)
    return [it for _, it in scored[:limit]]


def _search_meili(q, url, index="vus_docs", limit=30):
    try:
        u = url.rstrip("/") + "/indexes/" + urllib.parse.quote(index) + "/search"
        body = json.dumps({"q": q, "limit": limit}).encode("utf-8")
        req = urllib.request.Request(u, data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=8) as r:
            data = json.loads(r.read().decode("utf-8"))
        hits = data.get("hits") or []
        return [{"name": h.get("name", ""), "signature": h.get("signature", ""),
                 "doc": h.get("doc", ""), "example": h.get("example", ""),
                 "category": h.get("category", "")} for h in hits]
    except Exception:
        return None  # Meili 不可达 → 由调用方回退 local


def search_index(q, engine="local", meili_url=None):
    """默认本地兜底；meili_url 配置且 engine=meili 时尝试联调，失败回退本地。"""
    q = (q or "").strip()
    if not q:
        return {"engine": "local", "results": []}
    if engine == "meili" and meili_url:
        result = _search_meili(q, meili_url)
        if result is not None:
            return {"engine": "meili", "results": result}
    return {"engine": "local", "results": _search_local(q)}