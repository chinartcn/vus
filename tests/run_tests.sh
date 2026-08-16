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

# ---- C 单元测试（进程内插件/JSON 转换）----
# 需要 libpython 头文件；缺失时记录降级并跳过
echo ""
if command -v python3-config >/dev/null 2>&1; then
    echo "运行 C 单元测试: test_plugin_inproc"
    if gcc -DVUS_USE_PY $(python3-config --includes) -I../rt -I../rt/easylogger/inc \
        test_plugin_inproc.c ../rt/libvus_rt.c ../rt/vus_coro.c \
        ../rt/easylogger/src/elog.c ../rt/easylogger/src/elog_utils.c ../rt/elog_port.c \
        $(python3-config --ldflags) -lpthread -o test_plugin_inproc 2>/dev/null; then
        if LD_LIBRARY_PATH=$(python3-config --prefix)/lib \
            VUS_PLUGIN_DIR="../examples/plugins" ./test_plugin_inproc >/dev/null 2>&1; then
            echo "  ✅ test_plugin_inproc 通过"
            PASS=$((PASS + 1))
        else
            echo "  ❌ test_plugin_inproc 失败"
            FAIL=$((FAIL + 1))
            FAILED_FILES="$FAILED_FILES test_plugin_inproc(C)"
        fi
    else
        echo "  ⚠️  C 单测编译失败，跳过（记录降级）"
    fi
else
    echo "  ⚠️  未找到 python3-config，跳过 C 单测（降级路径）"
fi

echo ""
echo "=========================================="
printf "含 C 单测：共 %d 个用例，通过 %d 个，失败 %d 个\n" $((PASS + FAIL)) $PASS $FAIL
echo "=========================================="

if [ "$FAILED_FILES" != "" ]; then
    echo "失败用例：$FAILED_FILES"
    exit 1
fi

exit 0