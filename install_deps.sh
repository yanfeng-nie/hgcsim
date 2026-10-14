#!/usr/bin/env bash
# =============================================================================
# HGCSim 依赖安装（只负责安装依赖，不编译；编译请用 make build）
#
# 用法：
#   bash install_deps.sh                    # apt 安装编译工具 + 创建 .venv 并 pip 安装前端依赖
#   bash install_deps.sh --apt-only         # 只安装 apt 软件包
#   bash install_deps.sh --pip-only         # 只创建 .venv 并安装 Python 包（apt 包已具备时）
#   bash install_deps.sh --mirror URL       # pip 使用指定镜像，例如 https://pypi.tuna.tsinghua.edu.cn/simple
#   bash install_deps.sh --recreate-venv    # 删除并重建 .venv 后再安装 Python 包
#   bash install_deps.sh --dry-run          # 只打印将要执行的命令，不做任何修改
#   bash install_deps.sh --uninstall        # 撤销：删除 .venv，并卸载本脚本当初“新装”的 apt 软件包
#   bash install_deps.sh --force            # 非 Ubuntu 或低于 20.04 时也继续（不保证可用）
#   bash install_deps.sh -h | --help        # 显示本说明
#
# 安装内容：
#   apt ：build-essential（gcc/g++/make）、cmake、python3、python3-venv
#   pip ：requirements.txt 中的 streamlit / plotly / pandas，全部装进仓库内的 .venv/，
#         使用 --no-cache-dir，不写入系统 Python，也不在 ~/.cache/pip 留下缓存
# 记录：本脚本安装前后对比 dpkg 列表，把“原来没有、这次新装”的 apt 包（含自动依赖）
#       记到 .hgcsim_deps/apt_new_packages.txt；--uninstall 只卸载这份清单里的包，
#       安装前就已存在的软件包一律不动。
# =============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

VENV="$ROOT/.venv"
REC_DIR="$ROOT/.hgcsim_deps"
REC="$REC_DIR/apt_new_packages.txt"
APT_PKGS=(build-essential cmake python3 python3-venv)

DO_APT=1
DO_PIP=1
MIRROR=""
RECREATE=0
DRY=0
UNINSTALL=0
FORCE=0

usage() { awk 'NR>1 && /^#/ {sub(/^# ?/, ""); if ($0 !~ /^=+$/) print; next} NR>1 {exit}' "$0"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --apt-only) DO_PIP=0; shift ;;
    --pip-only) DO_APT=0; shift ;;
    --mirror) MIRROR="${2:-}"; [ -n "$MIRROR" ] || { echo "错误：--mirror 需要一个 URL" >&2; exit 2; }; shift 2 ;;
    --mirror=*) MIRROR="${1#--mirror=}"; shift ;;
    --recreate-venv) RECREATE=1; shift ;;
    --dry-run) DRY=1; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    --force) FORCE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：$1（用 --help 查看用法）" >&2; exit 2 ;;
  esac
done
if [ "$DO_APT" = 0 ] && [ "$DO_PIP" = 0 ]; then
  echo "错误：--apt-only 与 --pip-only 不能同时使用" >&2
  exit 2
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

run() {  # 打印并执行；--dry-run 时只打印
  echo "+ $*"
  [ "$DRY" = 1 ] || "$@"
}

installed_pkgs() {  # 当前已安装的软件包（含架构后缀，便于精确卸载）
  dpkg-query -W -f='${db:Status-Abbrev} ${binary:Package}\n' 2>/dev/null | awk '$1=="ii"{print $2}' | sort -u
}

# ---------------------------------------------------------------------------
# 撤销安装
# ---------------------------------------------------------------------------
if [ "$UNINSTALL" = 1 ]; then
  echo "== 撤销 HGCSim 依赖安装 =="
  if [ -d "$VENV" ]; then
    run rm -rf "$VENV"
  else
    echo "（没有 .venv，跳过）"
  fi
  if [ -s "$REC" ]; then
    mapfile -t RECORDED < <(sort -u "$REC")
    mapfile -t STILL < <(comm -12 <(printf '%s\n' "${RECORDED[@]}") <(installed_pkgs))
    if [ "${#STILL[@]}" -gt 0 ]; then
      echo "以下 apt 软件包是 install_deps.sh 当初新装的，将被卸载（apt 会列出明细并请你确认）："
      printf '  %s\n' "${STILL[@]}"
      run $SUDO apt-get remove --purge "${STILL[@]}"
    else
      echo "记录中的 apt 软件包都已不在系统中，跳过。"
    fi
  else
    echo "没有 apt 安装记录（.hgcsim_deps/apt_new_packages.txt）：说明这些软件包在运行本脚本前就已存在，"
    echo "或者是手动安装的。为避免误删，本脚本不卸载任何 apt 软件包。"
  fi
  [ "$DRY" = 1 ] || rm -rf "$REC_DIR"
  echo "撤销完成。编译产物可用 make clean 删除。"
  exit 0
fi

# ---------------------------------------------------------------------------
# 系统检查
# ---------------------------------------------------------------------------
OS_ID=""; OS_VER=""
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  OS_ID="${ID:-}"; OS_VER="${VERSION_ID:-}"
fi
ver_num() { local v="$1"; echo $(( ${v%%.*} * 100 + 10#${v#*.} )); }
if [ "$OS_ID" != "ubuntu" ] || [ -z "$OS_VER" ] || [ "$(ver_num "$OS_VER")" -lt 2004 ]; then
  echo "本项目只支持 Ubuntu 20.04 及以上（检测到：${PRETTY_NAME:-未知系统}）。" >&2
  if [ "$FORCE" = 1 ]; then
    echo "已指定 --force，继续安装（不保证可用）。" >&2
  else
    echo "如确需继续，请加 --force。" >&2
    exit 1
  fi
else
  echo "系统：${PRETTY_NAME:-Ubuntu $OS_VER}"
fi

# ---------------------------------------------------------------------------
# apt
# ---------------------------------------------------------------------------
if [ "$DO_APT" = 1 ]; then
  echo
  echo "== [1/2] apt 软件包：${APT_PKGS[*]} =="
  command -v apt-get >/dev/null 2>&1 || { echo "找不到 apt-get（不是 Ubuntu/Debian 系统？）" >&2; exit 1; }
  if [ "$DRY" = 0 ]; then
    BEFORE="$(mktemp)"
    trap 'rm -f "$BEFORE"' EXIT
    installed_pkgs > "$BEFORE"
  fi
  run $SUDO apt-get update
  run $SUDO apt-get install -y --no-install-recommends "${APT_PKGS[@]}"
  if [ "$DRY" = 0 ]; then
    NEW="$(comm -13 "$BEFORE" <(installed_pkgs) || true)"
    if [ -n "$NEW" ]; then
      mkdir -p "$REC_DIR"
      { [ -f "$REC" ] && cat "$REC"; echo "$NEW"; } | sort -u > "$REC.tmp" && mv "$REC.tmp" "$REC"
      echo "本次新装 $(echo "$NEW" | wc -l) 个 apt 软件包，已记录到 ${REC#"$ROOT"/}（--uninstall 时使用）"
    else
      echo "所需 apt 软件包均已存在，没有新装任何软件包。"
    fi
  fi
fi

# ---------------------------------------------------------------------------
# pip（仓库内虚拟环境 .venv）
# ---------------------------------------------------------------------------
if [ "$DO_PIP" = 1 ]; then
  echo
  echo "== [2/2] Python 依赖（安装到 ${VENV#"$ROOT"/}/）=="
  command -v python3 >/dev/null 2>&1 || { echo "找不到 python3，请先运行 bash install_deps.sh --apt-only" >&2; exit 1; }
  python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' \
    || { echo "需要 Python >= 3.8（当前 $(python3 -V 2>&1)）" >&2; exit 1; }
  if [ "$RECREATE" = 1 ] && [ -d "$VENV" ]; then
    run rm -rf "$VENV"
  fi
  if [ ! -x "$VENV/bin/python" ] || [ "$DRY" = 1 ]; then
    if ! run python3 -m venv "$VENV"; then
      echo "创建虚拟环境失败：缺少 python3-venv，请先运行 bash install_deps.sh --apt-only" >&2
      exit 1
    fi
  fi
  PIP_ARGS=(--no-cache-dir --disable-pip-version-check)
  [ -n "$MIRROR" ] && PIP_ARGS+=(-i "$MIRROR")
  run "$VENV/bin/python" -m pip install "${PIP_ARGS[@]}" --upgrade pip
  run "$VENV/bin/python" -m pip install "${PIP_ARGS[@]}" -r "$ROOT/requirements.txt"
  if [ "$DRY" = 0 ]; then
    "$VENV/bin/python" -c 'import streamlit, plotly, pandas; print("streamlit", streamlit.__version__, "| plotly", plotly.__version__, "| pandas", pandas.__version__)'
  fi
fi

echo
[ "$DRY" = 1 ] && { echo "（--dry-run：以上命令均未执行，没有做任何修改）"; exit 0; }
echo "依赖安装完成。下一步："
echo "  make build                     # 编译 C++ 模拟器（首次 build）"
echo "  bash scripts/run_frontend.sh   # 启动网页前端"
