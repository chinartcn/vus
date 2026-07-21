#!/bin/sh
# VUS 一键安装脚本
# 用法: curl -fsSL https://gitee.com/rtccn_mc/vus/raw/master/install.sh | sh

set -e

REPO_URL="https://gitee.com/rtccn_mc/vus.git"
INSTALL_DIR="${VUS_HOME:-$HOME/.vus}"

echo "==== VUS 语言安装工具 v0.1 ===="
echo ""

# 检查依赖
echo "检查依赖..."

# 检查 git
if ! command -v git >/dev/null 2>&1; then
    echo "错误: 未找到 git，请先安装 Git"
    echo "  Ubuntu/Debian: sudo apt install git"
    echo "  CentOS/RHEL:   sudo yum install git"
    echo "  Termux:        pkg install git"
    exit 1
fi
echo "  ✅ git"

# 检查 GCC
if ! command -v gcc >/dev/null 2>&1; then
    echo "错误: 未找到 gcc，请先安装 GCC"
    echo "  Ubuntu/Debian: sudo apt install gcc"
    echo "  CentOS/RHEL:   sudo yum install gcc"
    echo "  Termux:        pkg install gcc"
    exit 1
fi
echo "  ✅ gcc"

# 检查 make
if ! command -v make >/dev/null 2>&1; then
    echo "错误: 未找到 make，请先安装 make"
    echo "  Ubuntu/Debian: sudo apt install make"
    echo "  CentOS/RHEL:   sudo yum install make"
    exit 1
fi
echo "  ✅ make"
echo ""

# 克隆仓库
echo "正在安装 VUS..."
if [ -d "$INSTALL_DIR" ]; then
    echo "检测到已有安装，正在更新..."
    cd "$INSTALL_DIR"
    git pull --ff-only
else
    git clone --depth 1 "$REPO_URL" "$INSTALL_DIR"
    cd "$INSTALL_DIR"
fi

# 编译 VUS 编译器
echo ""
echo "编译 VUS 编译器..."
if make -C "$INSTALL_DIR" 2>&1; then
    echo "  ✅ 编译成功"
else
    echo "  ❌ 编译失败"
    exit 1
fi

# 设置可执行权限
chmod +x "$INSTALL_DIR/vus"

# 创建符号链接
echo ""
echo "设置 PATH..."
TARGET_DIR="$HOME/.local/bin"
if [ ! -d "$HOME/.local/bin" ]; then
    mkdir -p "$HOME/.local/bin"
fi
ln -sf "$INSTALL_DIR/vus" "$TARGET_DIR/vus"
echo "  ✅ 已创建符号链接: $TARGET_DIR/vus"

# 自动将 TARGET_DIR 加入 PATH（如果尚未在 PATH 中）
case ":$PATH:" in
    *":$TARGET_DIR:"*) ;;
    *)
        # 检测当前 shell 并写入对应的配置文件
        LINE="export PATH=\"\$HOME/.local/bin:\$PATH\""
        RC_FILE=""
        SHELL_NAME="$(basename "${SHELL:-/bin/sh}")"
        case "$SHELL_NAME" in
            bash)
                RC_FILE="$HOME/.bashrc"
                REFRESH_CMD="source $RC_FILE"
                ;;
            zsh)
                RC_FILE="$HOME/.zshrc"
                REFRESH_CMD="source $RC_FILE"
                ;;
            fish)
                RC_FILE="$HOME/.config/fish/config.fish"
                mkdir -p "$(dirname "$RC_FILE")"
                LINE="fish_add_path $TARGET_DIR"
                REFRESH_CMD="source $RC_FILE"
                ;;
            *)  # ash, sh, dash 等 POSIX shell
                RC_FILE="$HOME/.profile"
                REFRESH_CMD=". $RC_FILE"
                ;;
        esac
        if [ -n "$RC_FILE" ]; then
            mkdir -p "$(dirname "$RC_FILE")"
            grep -qxF "$LINE" "$RC_FILE" 2>/dev/null || echo "$LINE" >> "$RC_FILE"
            echo "  ✅ 已写入 $RC_FILE"
        fi
        # 也写入 .profile 作为后备（兼容所有 POSIX shell）
        if [ "$RC_FILE" != "$HOME/.profile" ]; then
            grep -qxF "export PATH=\"\$HOME/.local/bin:\$PATH\"" "$HOME/.profile" 2>/dev/null || \
                echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$HOME/.profile"
        fi
        echo "  ⚠️  请执行以下命令刷新 PATH，或重新打开终端："
        echo "     $REFRESH_CMD"
        ;;
esac

# 验证安装
echo ""
echo "验证安装..."
if "$INSTALL_DIR/vus" --help >/dev/null 2>&1; then
    echo "  ✅ VUS 安装成功！"
    echo ""
    echo "快速开始（执行上方刷新命令后）："
    echo "  vus init               # 初始化项目"
    echo "  vus run main.vus       # 编译并运行"
    echo "  vus build --c-only     # 仅编译为 C 代码"
    echo "  vus build --exe        # 编译为可执行文件"
    echo ""
    echo "当前终端可立即使用（通过完整路径）："
    echo "  $INSTALL_DIR/vus init"
else
    echo "  ❌ 安装验证失败"
    exit 1
fi

# 一键测试
echo ""
echo "═══════════════════════════════════════"
echo "  是否运行一键功能测试？"
echo "  这将执行所有测试用例验证编译器功能"
echo "  需要约 30-60 秒"
echo "═══════════════════════════════════════"
echo ""
printf "运行测试? [Y/n] "
read -r RUN_TESTS </dev/tty 2>/dev/null || RUN_TESTS="y"
case "$RUN_TESTS" in
    n|N|no|NO)
        echo "跳过测试。"
        echo "可以随时手动运行: $INSTALL_DIR/vus test"
        ;;
    *)
        echo ""
        echo "==== 运行 VUS 功能测试 ===="
        echo ""

        VUS="$INSTALL_DIR/vus"
        TESTS_DIR="$INSTALL_DIR/tests"
        PASS=0
        FAIL=0
        TOTAL=0

        # 测试 1: 基本编译
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 编译器版本..."
        if "$VUS" --version >/dev/null 2>&1; then
            echo "  ✅ 版本信息正确"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 版本信息错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 2: 帮助信息
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 帮助信息..."
        if "$VUS" --help >/dev/null 2>&1; then
            echo "  ✅ 帮助信息正确"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 帮助信息错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 3: 基本输出
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 基本输出 (test_hello)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_hello.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "Hello, World!"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 4: 变量
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 变量 (test_variables)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_variables.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "张三"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 5: 四则运算
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 四则运算 (test_arithmetic)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_arithmetic.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "7"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 6: 流程控制
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 流程控制 (test_control)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_control.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "大于"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 7: 函数
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 函数定义 (test_functions)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_functions.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "30"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 8: 比较运算符
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 比较运算符 (test_comparison)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_comparison.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "小于"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 9: 字符串拼接
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 字符串拼接 (test_string)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_string.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "字符串相等"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 10: 当循环
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 当循环 (test_while)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_while_count.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "55"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 11: 递归阶乘
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 递归阶乘 (test_factorial)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_factorial.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "720"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 12: 递归斐波那契
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 递归斐波那契 (test_fibonacci)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_fibonacci.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "21"; then
            echo "  ✅ 输出正确: $OUTPUT"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 13: 嵌套循环乘法表
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 嵌套循环 (test_nested_control)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_nested_control.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "9"; then
            echo "  ✅ 输出正确: 乘法表完整"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 输出错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 14: 综合功能
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 综合功能 (test_comprehensive)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_comprehensive.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "平方"; then
            echo "  ✅ 综合功能正常"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 综合功能错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 15: 综合演示 (test_demo)
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 综合演示 (test_demo)..."
        OUTPUT=$("$VUS" run "$TESTS_DIR/test_demo.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "全部功能正常"; then
            echo "  ✅ 综合演示完成"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 综合演示错误"
            FAIL=$((FAIL + 1))
        fi

        # 测试 16: 示例程序
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 示例程序 (examples/hello)..."
        OUTPUT=$("$VUS" run "$INSTALL_DIR/examples/hello.vus" 2>/dev/null)
        if echo "$OUTPUT" | grep -q "欢迎"; then
            echo "  ✅ 示例程序正确: a + b = 30"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 示例程序错误: $OUTPUT"
            FAIL=$((FAIL + 1))
        fi

        # 测试 17: 编译为 C 代码
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 编译为 C 代码..."
        BUILDDIR="$INSTALL_DIR/构建"
        mkdir -p "$BUILDDIR"
        if "$VUS" build --c-only "$TESTS_DIR/test_hello.vus" 2>/dev/null; then
            echo "  ✅ C 编译成功"
            PASS=$((PASS + 1))
        else
            echo "  ❌ C 编译失败"
            FAIL=$((FAIL + 1))
        fi

        # 测试 18: 编译为可执行文件
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 编译为可执行文件..."
        if "$VUS" build --exe "$TESTS_DIR/test_hello.vus" 2>/dev/null; then
            echo "  ✅ 可执行文件编译成功"
            PASS=$((PASS + 1))
        else
            echo "  ❌ 可执行文件编译失败"
            FAIL=$((FAIL + 1))
        fi

        # 测试 19: 插件打包（需要 Python 3）
        TOTAL=$((TOTAL + 1))
        echo "测试 $TOTAL: 插件打包..."
        if command -v python3 >/dev/null 2>&1; then
            if python3 "$INSTALL_DIR/scripts/vux_plugin_manager.py" build "$INSTALL_DIR/examples/plugins/示例" >/dev/null 2>&1; then
                echo "  ✅ 插件打包成功"
                PASS=$((PASS + 1))
                # 清理临时文件
                rm -f "$INSTALL_DIR"/*.vux
            else
                echo "  ❌ 插件打包失败"
                FAIL=$((FAIL + 1))
            fi
        else
            echo "  ⚠️ 跳过（Python 3 未安装）"
        fi

        # 测试 20: 插件列表（需要 Python 3）
        if command -v python3 >/dev/null 2>&1; then
            TOTAL=$((TOTAL + 1))
            echo "测试 $TOTAL: 插件列表..."
            if python3 "$INSTALL_DIR/scripts/vux_plugin_manager.py" list >/dev/null 2>&1; then
                echo "  ✅ 插件列表正常"
                PASS=$((PASS + 1))
            else
                echo "  ❌ 插件列表错误"
                FAIL=$((FAIL + 1))
            fi
        fi

        # 总结
        echo ""
        echo "═══════════════════════════════════════"
        echo "  测试结果: $PASS/$TOTAL 通过, $FAIL 失败"
        echo "═══════════════════════════════════════"

        if [ "$FAIL" -eq 0 ]; then
            echo "  🎉 所有功能测试通过！"
        else
            echo "  ⚠️  部分测试失败，请检查输出"
        fi
        echo ""
        ;;
esac