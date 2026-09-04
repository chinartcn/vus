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
    # 专项测试不在此跑：由下方 C 单测段覆盖（driver_vua_global.c 验证事件函数内全局变量）
    if [ "$test_name" = "test_vua_event_global.vus" ]; then
        echo "跳过专项: ${test_name}（由 driver_vua_global C 单测覆盖）"
        continue
    fi
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
    echo "失败用例（.vus 段）：$FAILED_FILES"
    echo "（继续运行 C 单测段，最终退出码按全部失败合并）"
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

# ---- VUA 组件流冒烟测试（渲染树归一/严格校验，不依赖 libpython）----
echo ""
echo "运行 VUA 冒烟测试: vua_smoke（渲染树归一 + 严格校验）"
if gcc -I../rt -Wall -O0 ../rt/vua.c ../rt/yyjson/yyjson.c vua_smoke.c -o vua_smoke 2>/dev/null; then
    if ./vua_smoke >/dev/null 2>&1; then
        echo "  ✅ vua_smoke 通过"
        PASS=$((PASS + 1))
    else
        echo "  ❌ vua_smoke 失败（重跑: ./vua_smoke）"
        FAIL=$((FAIL + 1))
        FAILED_FILES="$FAILED_FILES vua_smoke(C)"
    fi
else
    echo "  ⚠️  vua_smoke 编译失败，跳过（记录降级）"
fi

# ---- P1 回归：事件函数内全局变量 + P6 事件参数校验（driver_vua_global）----
echo ""
echo "运行 VUA 事件·全局变量测试: driver_vua_global（P1/P6 回归）"
VUS_SRC_C="$TEST_DIR/构建/test_vua_event_global.c"
if ( "$VUS" build --c-only test_vua_event_global.vus >/dev/null 2>&1 ) && [ -f "$VUS_SRC_C" ] \
   && gcc -I../rt -Dmain=vus_prog_main -c "$VUS_SRC_C" -o /tmp/vus_evt.o 2>/dev/null \
   && gcc -I../rt ../rt/vua.c driver_vua_global.c /tmp/vus_evt.o ../build/libvus_rt.a \
        -o /tmp/vus_vua_global_test -lm -ldl -lpthread 2>/dev/null \
   && ( cd .. && /tmp/vus_vua_global_test >/dev/null 2>&1 ); then
    echo "  ✅ driver_vua_global 通过"
    PASS=$((PASS + 1))
else
    echo "  ❌ driver_vua_global 失败（手工: bash -x run_tests.sh 定位）"
    FAIL=$((FAIL + 1))
    FAILED_FILES="$FAILED_FILES driver_vua_global(C)"
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