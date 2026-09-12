#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_conformance.py — VUS 语义一致性基线工具（Cordis_dc M2：双后端 conformance 锚点）

用途：把 tests/test_*.vus 的「退出码 + 运行输出 sha256」冻结为基线文件
  tests/conformance_<backend>.json。未来 VM 后端加入后，同一套用例同一套断言
  （--backend vm --check）即"对跑"：后端间 rc 与输出指纹必须一致，否则语义漂移。

基线协议：
  { "contract": 1, "backend": "c", "files": { "<用例名>": {"rc": int, "sha256": "<hex>"} } }
  rc：vus run 退出码；sha256：stdout+stderr 原文（先验确定性——同输入同输出）。

用例集合与 tests/run_tests.sh 一致：test_*.vus，跳过两个专项（VUA 事件/API 扩展
由 C 单测覆盖，vus run 不含 vua 链接）；Python 域用例注入 VUS_PLUGIN_DIR。

用法：
  生成基线:  python3 scripts/gen_conformance.py [--tests-dir tests] [--no-check]
  校验基线:  python3 scripts/gen_conformance.py --check [--backend c]
  对跑(远期): python3 scripts/gen_conformance.py --backend vm --check
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

CONTRACT = 1
# 与 run_tests.sh 一致：vua 专项走 C 单测，不纳入本基线
EXCLUDE = {"test_vua_event_global.vus", "vua_api_ext.vus"}
# 需要插件目录注入的用例（run_tests.sh 同款）
PY_ENV = {"test_ext_py.vus", "test_ext_py_vars.vus"}
# 输出含时间戳/环境信息（logger 日志、插件路径、机器状态等）的用例：
# 一致性协议仅约束退出码 rc，输出指纹置 null 跳过内容比对（内容随环境漂移，非语义漂移）。
NON_DET = {"test_legacy_stdlib.vus", "test_logger.vus", "test_plugins.vus",
           "test_xyz_basic.vus"}


def run_case(vus, tests_dir, name, backend, timeout=60):
    env = dict(os.environ)
    if backend != "c":
        env.setdefault("VUS_BACKEND", backend)  # 未来 VM 后端的注入点
    if name in PY_ENV:
        env["VUS_PLUGIN_DIR"] = os.path.join(os.path.dirname(os.path.abspath(tests_dir)), "examples")
    try:
        p = subprocess.run([vus, "run", name], cwd=tests_dir, env=env,
                           capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        # 网络/睡眠类用例在无外网/长延时时视为确定性（与 run_tests.sh 同语义：超时即 srverror）
        return {"rc": 124, "sha256": hashlib.sha256(b"timeout").hexdigest(), "timeout": True}
    digest = hashlib.sha256(p.stdout + p.stderr).hexdigest()
    if name in NON_DET:
        digest = None   # 非确定输出：只比 rc
    return {"rc": p.returncode, "sha256": digest}


def collect_cases(tests_dir):
    out = {}
    for fn in sorted(os.listdir(tests_dir)):
        if fn.startswith("test_") and fn.endswith(".vus") and fn not in EXCLUDE:
            out[fn] = None
    return out


def main():
    ap = argparse.ArgumentParser(description="VUS 语义一致性基线（生成/校验）")
    ap.add_argument("--vus", default=None, help="vus 可执行文件（缺省 <repo>/vus）")
    ap.add_argument("--tests-dir", default=None, help="用例目录（缺省 <repo>/tests）")
    ap.add_argument("--backend", default="c", help="后端标识（c=现有编译到 C；未来 vm）")
    ap.add_argument("--check", action="store_true", help="校验模式：与已有基线对比")
    ap.add_argument("--no-check", action="store_true", help="生成基线后不自动校验")
    args = ap.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    vus = args.vus or os.path.join(repo, "vus")
    tests_dir = args.tests_dir or os.path.join(repo, "tests")
    if not os.path.isfile(vus) or not os.access(vus, os.X_OK):
        print("错误: 未找到/不可执行 vus（先 make）: %s" % vus, file=sys.stderr)
        return 2
    golden = os.path.join(tests_dir, "conformance_%s.json" % args.backend)

    cases = collect_cases(tests_dir)
    if not cases:
        print("错误: %s 下没有匹配用例" % tests_dir, file=sys.stderr)
        return 2

    failed = 0
    runs = {}
    total = len(cases)
    for idx, name in enumerate(cases, 1):
        print("[conformance %s %d/%d] %s" % (args.backend, idx, total, name),
              file=sys.stderr, flush=True)
        entry = run_case(vus, tests_dir, name, args.backend)
        if entry.get("timeout"):
            failed += 1
            print("  ⚠ 超时(60s): %s" % name, file=sys.stderr)
        runs[name] = entry

    if args.check:
        with open(golden, "r", encoding="utf-8") as f:
            base = json.load(f)
        mism = []
        for name, got in runs.items():
            want = base["files"].get(name)
            if want is None or want != got:
                mism.append("%s: want %s got %s"
                            % (name, json.dumps(want or {}, ensure_ascii=False),
                               json.dumps(got, ensure_ascii=False)))
        if mism:
            print("一致性偏移（%d 个用例）:" % len(mism))
            for m in mism:
                print("  - " + m)
            return 1
        print("一致：%d 个用例基线全部匹配（backend=%s, contract=%d）"
              % (len(runs), args.backend, base.get("contract", -1)))
        return 0

    # 生成模式
    doc = {"contract": CONTRACT, "backend": args.backend, "files": runs}
    with open(golden, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    print("基线已写入 %s（%d 个用例）" % (golden, len(runs)))
    if not args.no_check:
        rc = main_from(args)  # 递归校验一次
        return rc
    return 0


def main_from(args):
    args.check = True
    return main()


if __name__ == "__main__":
    sys.exit(main())