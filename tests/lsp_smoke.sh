#!/bin/bash
# VUS LSP 一次性冒烟测试
# 通过管道向 `./vus lsp` 发送一组 JSON-RPC 请求（Content-Length 分帧），
# 断言三层补全 + initialize + executeCommand + shutdown + exit 的响应符合预期，
# 且进程以退出码 0 优雅退出。
set -e
cd "$(dirname "$0")"

VUS="../vus"

# Content-Length 分帧辅助（请求体会包含中文，须以“字节数”计长，不能直接用 ${#}）
frame() { local body="$1"; local len; len=$(printf '%s' "$body" | wc -c | tr -d ' '); printf 'Content-Length: %d\r\n\r\n%s' "$len" "$body"; }

# 三段补全共用的文档与光标位置
INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'

# 1) 普通补全：光标所在行为 `图形_`，光标在其末尾（character=7）
NORMAL='{"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"text":"图形_"},"position":{"line":0,"character":7}}}'

# 2) 详细补全：光标所在行为 `.:图形_滚动容器`
DETAIL='{"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"text":".:图形_滚动容器"},"position":{"line":0,"character":25}}}'

# 3) 命令补全：光标所在行为 `..:执行 开始`
COMMAND='{"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"text":"..:执行 开始"},"position":{"line":0,"character":25}}}'

# 3.5) VUA 普通补全：光标所在行为 `界面_`，光标在其末尾（character=7）
VUA_NORMAL='{"jsonrpc":"2.0","id":5,"method":"textDocument/completion","params":{"textDocument":{"text":"界面_"},"position":{"line":0,"character":7}}}'

# workspace/executeCommand：处理 `开始` 命令，stdout 打印执行意图并返回成功
EXEC='{"jsonrpc":"2.0","id":6,"method":"workspace/executeCommand","params":{"command":"开始"}}'

SHUT='{"jsonrpc":"2.0","id":7,"method":"shutdown"}'
EXIT='{"jsonrpc":"2.0","method":"exit"}'

# 4) .vua 校验闭环：didOpen 坏 JSON 应发布 publishDiagnostics（source=vua-lint）
VUA_OPEN='{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/坏.vua","languageId":"vua","version":1,"text":"{非法JSON"}}}'

PAYLOAD="$(frame "$INIT")$(frame "$NORMAL")$(frame "$DETAIL")$(frame "$COMMAND")$(frame "$VUA_NORMAL")$(frame "$EXEC")$(frame "$VUA_OPEN")$(frame "$SHUT")$(frame "$EXIT")"

OUTPUT="$(printf '%s' "$PAYLOAD" | "$VUS" lsp 2>&1)"
RC=$?

echo "---------- LSP 原始输出 ----------"
echo "$OUTPUT"
echo "----------------------------------"

fail() { echo "❌ 断言失败: $1"; exit 1; }

if [ "$RC" -ne 0 ]; then fail "预期退出码 0，实际 $RC"; fi

# initialize
echo "$OUTPUT"  | grep -q '"vus-lsp"'  || fail "initialize 未返回 serverInfo.name=vus-lsp"
echo "$OUTPUT"  | grep -q '"0.1.0"'    || fail "initialize 未返回 serverInfo.version=0.1.0"
echo "$OUTPUT"  | grep -q 'triggerCharacters' || fail "initialize 未返回 completionProvider.triggerCharacters"

# 普通补全：图形_ 前缀应返回 图形_矩形 等内置函数（kind=3）
grep -q '"图形_矩形"' <<< "$OUTPUT" || fail "普通补全未返回 图形_矩形"
grep -q '"图形_初始化"' <<< "$OUTPUT" || fail "普通补全未返回 图形_初始化"
grep -q '"kind":3' <<< "$OUTPUT" || fail "普通补全未返回函数 kind=3"

# 详细补全：应返回完整签名（detail）与 documentation
grep -q '图形_滚动容器(名, x, y, 宽, 高, 内容高)' <<< "$OUTPUT" || fail "详细补全未返回 图形_滚动容器 完整签名"
grep -q '"documentation"' <<< "$OUTPUT" || fail "详细补全未返回 documentation 字段"

# 命令补全：应返回命令 `开始`（kind=9），并打印执行意图
grep -q '"开始"' <<< "$OUTPUT" || fail "命令补全未返回命令 开始"
grep -q '"kind":9' <<< "$OUTPUT" || fail "命令补全未返回命令 kind=9"
grep -q '执行命令: 开始' <<< "$OUTPUT" || fail "executeCommand 未在 stdout 打印执行意图"

# VUA 普通补全：界面_ 前缀应返回 VUA 内建（kind=3）
grep -q '"界面_显示"' <<< "$OUTPUT" || fail "VUA 补全未返回 界面_显示"
grep -q '"界面_全局取"' <<< "$OUTPUT" || fail "VUA 补全未返回 界面_全局取"

# .vua 校验闭环：didOpen 坏 JSON → publishDiagnostics（source=vua-lint）
grep -q 'textDocument/publishDiagnostics' <<< "$OUTPUT" || fail ".vua didOpen 未发布 publishDiagnostics"
grep -q '"source":"vua-lint"' <<< "$OUTPUT" || fail ".vua 诊断 source 非 vua-lint"
grep -q '非法 JSON' <<< "$OUTPUT" || fail ".vua 诊断未含 JSON 解析错误"

# shutdown：应返回 result null
grep -q '"result":null' <<< "$OUTPUT" || fail "shutdown 未返回 result:null"

echo "✅ 冒烟测试全部通过（退出码 $RC）"
exit 0