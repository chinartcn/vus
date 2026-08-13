#!/bin/bash
# VUS 错误测试运行脚本
# 运行故意包含错误的测试文件，验证编译器正确报告错误
# 用法: cd tests && bash run_error_tests.sh
VUS="../vus"
ERROR_DIR="error_tests"
PASS=0
FAIL=0
TOTAL=0

echo "=========================================="
echo "  VUS 编译器错误测试套件"
echo "  (这些测试预期编译失败)"
echo "=========================================="
echo ""

# 检查编译器是否存在
if [ ! -f "$VUS" ]; then
    echo "错误: 未找到编译器 ($VUS)"
    echo "请先在项目根目录执行 make"
    exit 1
fi

# 检查编译器是否可执行
if [ ! -x "$VUS" ]; then
    echo "错误: 编译器不可执行 ($VUS)"
    exit 1
fi

# 检查错误测试目录
if [ ! -d "$ERROR_DIR" ]; then
    echo "错误: 未找到错误测试目录 ($ERROR_DIR)"
    exit 1
fi

for test_file in "${ERROR_DIR}/test_"*.vus; do
    # 检查是否有匹配的文件
    if [ ! -f "$test_file" ]; then
        echo "未找到错误测试文件 (*.vus)"
        exit 1
    fi

    test_name="$(basename "$test_file")"
    TOTAL=$((TOTAL + 1))
    printf "错误测试: %-35s" "$test_name"

    # 编译运行，预期失败
    if output="$("$VUS" run "$test_file" 2>&1)"; then
        echo "❌ 意外通过 (应该失败)"
        FAIL=$((FAIL + 1))
    else
        exit_code=$?
        echo "✅ 正确检测到错误 (退出码: $exit_code)"
        PASS=$((PASS + 1))
    fi
done

echo ""
echo "=========================================="
printf "错误测试完成：共 %d 个用例，正确检测 %d 个，漏检 %d 个\n" $TOTAL $PASS $FAIL
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "警告：有错误测试用例未被编译器正确检测！"
    exit 1
fi

echo "所有错误均被编译器正确检测！"
exit 0