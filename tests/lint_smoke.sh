#!/bin/bash
# VUS vus lint 冒烟测试：独立 .vua 校验命令（复用 rt/vua.c 严格校验+渲染树归一）
# 用法: cd tests && bash lint_smoke.sh
# 依赖 ./vus（lint_smoke.sh 定位 ../vus，需先 make）+ tests/lint/*.vua fixtures
set -e
cd "$(dirname "$0")"
VUS="../vus"
CONTROLS="../testdata/vua_controls.json"

fail() { echo "❌ 断言失败: $1"; exit 1; }

# 1) 好文件应通过（显式控件表）
if OUTPUT="$("$VUS" lint --controls "$CONTROLS" lint/good.vua 2>&1)"; then
    echo "$OUTPUT" | grep -q "校验通过: lint/good.vua" || fail "好文件未输出校验通过"
else
    fail "好文件 lint 意外失败: $OUTPUT"
fi

# 2) 坏 JSON / 坏根应失败且退出码非 0
if OUTPUT="$("$VUS" lint --controls "$CONTROLS" lint/bad_json.vua 2>&1)"; then
    fail "坏 JSON lint 意外通过: $OUTPUT"
fi
echo "$OUTPUT" | grep -q "非法 JSON" || fail "坏 JSON 未报非法 JSON"

if OUTPUT="$("$VUS" lint --controls "$CONTROLS" lint/bad_root.vua 2>&1)"; then
    fail "坏根 lint 意外通过: $OUTPUT"
fi
echo "$OUTPUT" | grep -q "根节点的 type 必须是「界面」" || fail "坏根未报根节点错误"

# 3) 批量：好+坏混合退出码为 1
if "$VUS" lint --controls "$CONTROLS" lint/good.vua lint/bad_json.vua >/dev/null 2>&1; then
    fail "混合 lint 应退出码非 0"
fi

echo "✅ vus lint 冒烟测试通过"
exit 0