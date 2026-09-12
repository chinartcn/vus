#!/bin/bash
# 全面能力测试（排除 GUI 与 VUA）
# 按能力域分组运行 tests/ 下的非 GUI/vua 用例，输出明细与汇总。
# 用法: cd tests && bash run_abilities.sh
VUS="../vus"
PASS=0; FAIL=0; TOTAL=0
FAILED=""

run_one() { # 文件 [env前缀]
    local f="$1" extra="$2" name
    name="$(basename "$f")"
    printf "  %-40s" "$name"
    local out ec
    if [ -n "$extra" ]; then
        out="$($extra "$VUS" run "$f" 2>&1)"; ec=$?
    else
        out="$("$VUS" run "$f" 2>&1)"; ec=$?
    fi
    TOTAL=$((TOTAL+1))
    if [ $ec -eq 0 ]; then
        echo "✅ 通过"
        PASS=$((PASS+1))
    else
        echo "❌ 失败 (exit=$ec)"
        echo "$out" | head -n 6
        FAIL=$((FAIL+1))
        FAILED="$FAILED $name"
    fi
}

if [ ! -x "$VUS" ]; then
    echo "错误: 未找到编译器 ($VUS)，请先 make"
    exit 1
fi

# 能力域：名称:文件列表（全部相对 tests/）
DOMAINS=(
  "语言核心-基础:test_hello.vus test_demo.vus test_comprehensive.vus"
  "语言核心-运算符:test_arithmetic.vus test_bitwise.vus test_comparison.vus test_logical.vus test_not.vus test_not_operator.vus test_negative.vus test_modulo.vus test_composite_assign.vus test_expr_stmt.vus test_tonumber_reg.vus"
  "语言核心-变量作用域:test_variables.vus test_local_vars.vus test_block_scope.vus test_param_shadow.vus test_global.vus test_toplevel_global.vus test_global_event.vus test_null.vus"
  "语言核心-控制流:test_control.vus test_elif.vus test_break_continue.vus test_while_count.vus test_nested_control.vus test_nested_control_2.vus"
  "语言核心-字符串容器:test_string.vus test_concat.vus test_fstring.vus test_literal.vus test_subscript.vus test_dict_iterate.vus test_json_dict.vus test_objtext_json.vus"
  "语言核心-函数:test_functions.vus test_factorial.vus test_fibonacci.vus test_recursion.vus test_multi_return.vus test_function_value.vus"
  "语言核心-泛型结构体:test_generic.vus test_generic_call.vus test_generic_mono.vus test_generic_unused.vus test_type_annot.vus test_struct_basic.vus test_struct_chain.vus test_cast.vus"
  "运行时-并发内存:test_await.vus test_await_multi.vus test_thread_coro.vus test_container_release.vus test_r6_forward_chain.vus test_platform_light.vus"
  "异常系统:test_error.vus test_exception.vus test_exception_types.vus"
  "内置标准库:test_ext_builtins.vus test_legacy_stdlib.vus test_file_io.vus test_network.vus test_logger.vus test_sleep.vus test_tui.vus test_tui_editor.vus test_ai_agg.vus"
  "导入多模块:test_import.vus test_pages_ext.vus"
  "FFI-插件-ABI:test_ext_c.vus test_plugin_run_json.vus test_plugins.vus plugin_realtime_call.vus test_vus_abi.vus test_vus_chart.vus test_vus_lang.vus test_vus_plugin.vus"
  "Cordis 元框架:test_cordis.vus test_cordis_multi.vus"
  "专项-体感XYZ:test_xyz_basic.vus"
)

for d in "${DOMAINS[@]}"; do
    name="${d%%:*}"
    files="${d#*:}"
    echo ""
    echo "===== $name ====="
    for f in $files; do
        run_one "$f" ""
    done
done

# Python ABI 域需要注入插件搜索路径；test_py_game 还需 .vux 插件就位于 ~/.vus/plugins
echo ""
echo "===== FFI-Python 插件路径注入 ====="
mkdir -p "$HOME/.vus/plugins"
cp -r ../examples/plugins/猜数游戏 "$HOME/.vus/plugins/" 2>/dev/null
run_one "test_ext_py.vus" "env VUS_PLUGIN_DIR=../examples"
run_one "test_ext_py_vars.vus" "env VUS_PLUGIN_DIR=../examples"
run_one "test_py_game.vus" "env VUS_PLUGIN_DIR=../examples"

# 错误检测域：预期编译失败
echo ""
echo "===== 错误检测（预期编译失败） ====="
for f in error_tests/test_*.vus; do
    name="$(basename "$f")"
    printf "  %-40s" "$name"
    if out="$("$VUS" run "$f" 2>&1)"; then
        echo "❌ 意外通过（应报错）"
        FAIL=$((FAIL+1))
        FAILED="$FAILED $name"
    else
        echo "✅ 正确拒绝"
        PASS=$((PASS+1))
    fi
    TOTAL=$((TOTAL+1))
done

echo ""
echo "=========================================="
printf "全面能力测试：共 %d 项，通过 %d，失败 %d\n" "$TOTAL" "$PASS" "$FAIL"
echo "=========================================="
if [ -n "$FAILED" ]; then
    echo "失败项：$FAILED"
    exit 1
fi
echo "全部能力域通过（排除 GUI 与 VUA）"
exit 0
