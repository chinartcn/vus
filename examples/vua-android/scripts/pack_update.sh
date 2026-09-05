#!/usr/bin/env bash
# pack_update.sh — 打包 VUS 热更更新包（zip + manifest.json + 逐文件 sha256）
#
# 依据 docs/designs/2026-09-05-hotupdate-loader-design.md §7：
#   收集 .so(多ABI) + plugins/*.dex + assets 全量 → manifest.json
#   （版本 / 最低宿主版本 / 文件 sha256）→ zip 整包 → dist/vus_update_<版本>.zip。
#   同一份包既可发布到更新服务，也可由桌面 CLI"目录替换 + 重启"调试（协议两端共用 manifest）。
#
# 用法: scripts/pack_update.sh --version 1.0.1 [--min-version 12] [--out <dir>]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${VERSION:-}"
MIN="${MIN_HOST_VERSION:-0}"
OUT="${OUT:-$ROOT/dist}"

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --min-version) MIN="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "未知参数: $1 （用法: $0 --version N [--min-version N] [--out DIR]）"; exit 1 ;;
  esac
done
[ -n "$VERSION" ] || { echo "需 --version <版本号>"; exit 1; }

WORK="$ROOT/build/update_pkg"
rm -rf "$WORK"; mkdir -p "$WORK" "$OUT"

# 1) 收集文件（zip 内相对路径 = filesDir 相对路径）
#    .so（多 ABI）：build/libs/<abi>/libvus_app.so → lib/<abi>/
if [ -d "$ROOT/build/libs" ]; then
  mkdir -p "$WORK/lib"
  cp -r "$ROOT/build/libs"/. "$WORK/lib/"
fi
#    逻辑插件 dex：plugins/dist/*.dex → plugins/
if ls "$ROOT/plugins/dist"/*.dex >/dev/null 2>&1; then
  mkdir -p "$WORK/plugins"
  cp "$ROOT/plugins/dist"/*.dex "$WORK/plugins/"
fi
#    assets 全量（.vua/.json/图片/插件等，相对路径原样 = filesDir 相对路径）
if [ -d "$ROOT/app/src/main/assets" ]; then
  ( cd "$ROOT/app/src/main/assets" && cp -r . "$WORK" )
fi

# 2) 生成 manifest.json（版本 / 最低宿主版本 / 文件 sha256），放进 zip 根
python3 - "$VERSION" "$MIN" "$WORK" > "$WORK/manifest.json" <<'PY'
import sys, hashlib, os, json
ver, mini, work = sys.argv[1], sys.argv[2], sys.argv[3]
files = {}
for root, _dirs, names in os.walk(work):
    for n in names:
        p = os.path.join(root, n)
        rel = os.path.relpath(p, work).replace(os.sep, "/")
        if rel == "manifest.json":
            continue
        with open(p, "rb") as f:
            h = hashlib.sha256(f.read()).hexdigest()
        files[rel] = "sha256:" + h
print(json.dumps({"版本": int(ver), "最低宿主版本": int(mini), "文件": files},
                 ensure_ascii=False, indent=2))
PY

# 3) 打包 zip（manifest 放根）→ dist/vus_update_<版本>.zip；独立 manifest 输出便于"只拉清单"
ZIP="$OUT/vus_update_$VERSION.zip"
( cd "$WORK" && zip -q -r "$ZIP" . )
cp "$WORK/manifest.json" "$OUT/manifest_$VERSION.json"

echo "== 完成: $ZIP =="
ls -lh "$ZIP" "$OUT/manifest_$VERSION.json"