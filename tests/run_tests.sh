#!/bin/bash
# VUS 测试运行脚本
# 用法: cd tests && bash run_tests.sh
set -e

VUS="../vus"
TEST_DIR="."
PASS=0
FAIL=0
FAILED_FILES=""

echo "=========================================="
echo "  VUS 编译器测试套件"
echo "=========================================="
echo ""

# 检查编译器是否存在
if [ ! -f "$VUS" ]; then
    echo "错误: 未找到编译器 ($VUS)"
    echo "请先在项目根目录执行 make"
    exit 1
fi

# 检查 VUS 是否可执行
if [ ! -x "$VUS" ]; then
    echo "错误: 编译器不可执行 ($VUS)"
    exit 1
fi

for test_file in "${TEST_DIR}/test_"*.vus; do
    # 检查是否有匹配的文件
    if [ ! -f "$test_file" ]; then
        echo "未找到测试文件 (*.vus)"
        exit 1
    fi

    test_name="$(basename "$test_file")"
    printf "运行测试: %-30s" "$test_name"

    # 编译并运行 VUS 文件
    # 先尝试 build --c-only，再编译运行；或直接 run
    if output="$("$VUS" run "$test_file" 2>&1)"; then
        echo "✅ 通过"
        PASS=$((PASS + 1))
    else
        exit_code=$?
        echo "❌ 失败 (退出码: $exit_code)"
        echo "--- 输出 ---"
        echo "$output"
        echo "------------"
        FAIL=$((FAIL + 1))
        FAILED_FILES="$FAILED_FILES $test_name"
    fi
done

echo ""
echo "=========================================="
printf "测试完成：共 %d 个用例，通过 %d 个，失败 %d 个\n" $((PASS + FAIL)) $PASS $FAIL
echo "=========================================="

if [ "$FAILED_FILES" != "" ]; then
    echo "失败用例：$FAILED_FILES"
    exit 1
fi

exit 0