#!/usr/bin/env bash
# =============================================================================
# 全流程自检（不编译，检查的是当前 build；先 make build 或 make build BUILD_NAME=<build名>）
#
# 用法：
#   bash scripts/run_tests.sh                   # 全部检查
#   bash scripts/run_tests.sh --skip-frontend   # 不运行前端冒烟测试（未安装 streamlit 时可用）
#   bash scripts/run_tests.sh -h | --help
#
# 依次运行：
#   1. C++ 单元测试（ctest）
#   2. build 名审查测试（tests/test_build_names.sh，只跑 CMake 配置阶段，不影响 build/）
#   3. 命令行仿真一次（build/hgcsim_cli）
#   4. 基线测试套件（结果写到 build/suite_results.json，要求 5 分钟内完成）
#   5. 前端冒烟测试（tests/test_frontend.py，使用临时目录，不改动 configs/user/）
# =============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
FRONTEND=1
for a in "$@"; do
  case "$a" in
    --skip-frontend) FRONTEND=0 ;;
    -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/, ""); if ($0 !~ /^=+$/) print; next} NR>1 {exit}' "$0"; exit 0 ;;
    *) echo "未知参数：$a（用 --help 查看用法）" >&2; exit 2 ;;
  esac
done
[ -f .venv/bin/activate ] && . .venv/bin/activate
[ -f build/libhgcsim.so ] && [ -f build/CTestTestfile.cmake ] \
  || { echo "错误：还没有 build，请先运行 make build（或 make build BUILD_NAME=<build名>）" >&2; exit 1; }
export PYTHONPATH="$ROOT/frontend${PYTHONPATH:+:$PYTHONPATH}"
if ! python3 -c 'import sys; from hgcsim_frontend import buildinfo; sys.exit(1 if buildinfo.info()["stale"] else 0)'; then
  echo "错误：源码或 plugins/student/ 在当前 build 之后被修改过，请先重新编译：make build BUILD_NAME=<build名>" >&2
  exit 1
fi
NAME="$(sed -n 's/.*"name": "\(.*\)".*/\1/p' build/build_info.json | head -1)"
echo "== 自检当前 build：$NAME =="

echo; echo "== [1/5] C++ 单元测试 =="
(cd build && ctest --output-on-failure)

echo; echo "== [2/5] build 名审查 =="
bash tests/test_build_names.sh

echo; echo "== [3/5] 命令行仿真 =="
./build/hgcsim_cli run configs/systems/hetero_lab.json configs/workloads/B5.json configs/strategies/naive_gpu.json

echo; echo "== [4/5] 基线测试套件 =="
python3 -m hgcsim_frontend.suite --out build/suite_results.json

if [ "$FRONTEND" = 1 ]; then
  echo; echo "== [5/5] 前端冒烟测试 =="
  python3 tests/test_frontend.py
else
  echo; echo "== [5/5] 跳过前端冒烟测试（--skip-frontend）=="
fi
echo
echo "全部自检通过（build：$NAME）"
