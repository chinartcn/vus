#!/bin/bash
# VUS LSP 高级自测：文档缓冲 + 精确符号收集 + documentSymbol
# 使用 python3 构造 JSON-RPC 请求序列（didOpen → completion → documentSymbol），
# 逐帧按 Content-Length（bytes 偏移）严格解析响应，断言：
#   - 文档缓冲按 uri 生效：completion 普通补全出现函数 `页_首页`(kind=3) 与变量 `x`(kind=6)
#   - documentSymbol 返回 `页_首页`(kind=12) 与 `x`(kind=13)，且带范围 range
#   - 每条响应均为合法 JSON
set -e
cd "$(dirname "$0")"

python3 - <<'PY'
import json
import subprocess
import sys

# ---------- Content-Length 分帧构造 ----------
def frame(body: str) -> bytes:
    raw = body.encode('utf-8')          # 必须按字节数计长
    return b"Content-Length: %d\r\n\r\n" % len(raw) + raw

# ---------- 严格按 bytes 偏移解析响应帧 ----------
def parse_frames(data: bytes):
    frames = []
    i, n = 0, len(data)
    while i < n:
        start = data.find(b"Content-Length:", i)
        if start < 0:
            break
        j = start + len(b"Content-Length:")
        while j < n and data[j] == 0x20:          # 跳过可选的空格
            j += 1
        k = j
        while k < n and data[k] not in (0x0d, 0x0a):
            k += 1
        length = int(data[j:k].decode('ascii'))
        # 头部行结束：\r\n\r\n 或 \n\n
        if data[k:k + 2] == b'\r\n':
            k += 2
            if data[k:k + 2] == b'\r\n':
                k += 2
            elif data[k:k + 1] == b'\n':
                k += 1
        elif data[k:k + 1] == b'\n':
            k += 1
            if data[k:k + 1] == b'\n':
                k += 1
        else:
            raise ValueError("非法响应头（缺少空行分隔）")
        body = data[k:k + length]
        if len(body) != length:
            raise ValueError("响应体字节数不足：声明 %d 实得 %d" % (length, len(body)))
        frames.append(body)
        i = k + length
    return frames

# ---------- 构造请求序列 ----------
init = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}}

uri = "file:///tmp/demo.vus"
doc_text = "定义 页_首页():\n    图形_初始化(320, 240, \"示例\")\n    x = 1\n\n"

# didOpen（通知，无 id）
did_open = {"jsonrpc": "2.0", "method": "textDocument/didOpen",
            "params": {"textDocument": {"uri": uri,
                                        "languageId": "vus",
                                        "version": 1,
                                        "text": doc_text}}}

# 普通补全：光标在最后一个空行（extract 不到有效前缀 → token 为空 → 列出全部候选）
completion = {"jsonrpc": "2.0", "id": 2,
              "method": "textDocument/completion",
              "params": {"textDocument": {"uri": uri},
                         "position": {"line": 3, "character": 0}}}

# documentSymbol：不带 request 文本，完全依赖 uri 缓冲
doc_symbol = {"jsonrpc": "2.0", "id": 3,
              "method": "textDocument/documentSymbol",
              "params": {"textDocument": {"uri": uri}}}

shutdown = {"jsonrpc": "2.0", "id": 4, "method": "shutdown"}
exit_req = {"jsonrpc": "2.0", "method": "exit"}

payload = b"".join(frame(json.dumps(r, ensure_ascii=False))
                   for r in (init, did_open, completion, doc_symbol, shutdown, exit_req))

# ---------- 执行 ----------
proc = subprocess.run(["./../vus", "lsp"], input=payload,
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
if proc.returncode != 0:
    print("断言失败: vus lsp 退出码 %d" % proc.returncode)
    print(proc.stderr.decode('utf-8', 'replace'))
    sys.exit(1)

frames = parse_frames(proc.stdout)
# 响应帧按发出顺序：id1=initialize, id2=completion, id3=documentSymbol, id4=shutdown
assert len(frames) == 4, "应收到 4 条响应，实收 %d" % len(frames)

def resp_by_id(target):
    for fr in frames:
        doc = json.loads(fr)            # 解析失败会抛异常 → 断言非法 JSON
        if doc.get("id") == target:
            return doc
    raise AssertionError("未找到 id=%d 的响应" % target)

init_resp = resp_by_id(1)
caps = init_resp["result"]["capabilities"]
assert caps.get("documentSymbolProvider") is True, "未声明 documentSymbolProvider=true"

# ---- completion：应含函数 页_首页(kind=3) 与变量 x(kind=6)，isIncomplete=false ----
comp = resp_by_id(2)["result"]
assert comp.get("isIncomplete") is False, "completion 必须 isIncomplete=false"
items = comp["items"]
assert isinstance(items, list) and len(items) > 0
kinds = {(it.get("label"), it.get("kind")) for it in items}
assert ("页_首页", 3) in kinds, "completion 未返回函数 页_首页(kind=3)"
assert ("x", 6) in kinds, "completion 未返回变量 x(kind=6)"

# ---- documentSymbol：含 页_首页(kind=12) 与 x(kind=13)，带范围 ----
syms = resp_by_id(3)["result"]
assert isinstance(syms, list), "documentSymbol 结果应为数组"
smap = {s["name"]: s for s in syms}
assert "页_首页" in smap, "documentSymbol 缺少 页_首页"
assert smap["页_首页"]["kind"] == 12, "页_首页 应为 Function(12)"
assert "x" in smap, "documentSymbol 缺少 x"
assert smap["x"]["kind"] == 13, "x 应为 Variable(13)"
for s in syms:
    r = s["range"]
    assert r["start"]["line"] >= 0 and r["start"]["character"] >= 0, "符号范围非法"
    assert r["end"]["line"] >= r["start"]["line"], "符号结束行应不小于起始行"

shut = resp_by_id(4)
assert shut["result"] is None, "shutdown 应返回 null"

print("LSP 高级自测全部通过：")
print("  - didOpen 缓冲按 uri 生效，completion 返回 页_首页(Function) 与 x(Variable)")
print("  - documentSymbol 返回 页_首页(kind=12) 与 x(kind=13)，均带合法范围")
print("  - 4 条响应 JSON 均合法，Content-Length 字节偏移解析一致")
PY