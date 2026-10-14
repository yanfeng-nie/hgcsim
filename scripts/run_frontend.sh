#!/usr/bin/env bash
# =============================================================================
# 启动 HGCSim 网页前端（Streamlit），默认地址 http://localhost:8501
#
# 用法：
#   bash scripts/run_frontend.sh                       # 默认端口 8501，监听所有网卡
#   bash scripts/run_frontend.sh --port 14541          # 指定端口
#   bash scripts/run_frontend.sh --address 127.0.0.1   # 只允许本机访问
#   bash scripts/run_frontend.sh -- --server.maxUploadSize 50   # “--” 之后的参数原样传给 streamlit
#   bash scripts/run_frontend.sh -h | --help
#
# 前提：已运行 bash install_deps.sh（安装 Python 依赖）与 make build（编译模拟器）。
# 重新 make build 之后需要按 Ctrl+C 停止前端并重新运行本脚本，前端才会加载新的 build。
# =============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PORT=8501
ADDR=""
EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --port|-p) PORT="${2:-}"; shift; [ $# -gt 0 ] && shift ;;
    --port=*) PORT="${1#--port=}"; shift ;;
    --address|-a) ADDR="${2:-}"; shift; [ $# -gt 0 ] && shift ;;
    --address=*) ADDR="${1#--address=}"; shift ;;
    -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/, ""); if ($0 !~ /^=+$/) print; next} NR>1 {exit}' "$0"; exit 0 ;;
    --) shift; EXTRA+=("$@"); break ;;
    *) EXTRA+=("$1"); shift ;;
  esac
done
[[ "$PORT" =~ ^[0-9]+$ ]] && [ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || { echo "错误：端口必须是 1~65535 的整数" >&2; exit 2; }

[ -f .venv/bin/activate ] && . .venv/bin/activate
python3 -c 'import streamlit, plotly, pandas' 2>/dev/null \
  || { echo "错误：缺少 Python 依赖（streamlit/plotly/pandas），请先运行 bash install_deps.sh" >&2; exit 1; }
[ -f build/libhgcsim.so ] \
  || { echo "错误：还没有编译模拟器（找不到 build/libhgcsim.so），请先运行 make build" >&2; exit 1; }

ARGS=(--server.port "$PORT" --server.headless true --theme.base light)
[ -n "$ADDR" ] && ARGS+=(--server.address "$ADDR")
echo "HGCSim 前端：http://localhost:$PORT  （按 Ctrl+C 停止）"
exec python3 -m streamlit run frontend/app.py "${ARGS[@]}" ${EXTRA[@]+"${EXTRA[@]}"}
