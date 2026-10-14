#!/usr/bin/env bash
# =============================================================================
# 命令行运行基线测试套件（不需要前端）：B1–B6 + R-MAT 可扩展性扫描，墙钟上限 5 分钟
#
# 用法：bash scripts/run_suite.sh [选项]          （选项原样传给 python3 -m hgcsim_frontend.suite）
#   （无选项）                         默认组合：系统 default_pc、hetero_lab × 套件默认策略
#   --list                             列出可用的系统、策略（内置 + 我的配置）与任务，然后退出
#   --systems  名称 ...                系统配置名（configs/systems/ 或 configs/user/systems/ 中的文件名，不含 .json）
#   --strategies 名称 ...              策略名（configs/strategies/ 或 configs/user/strategies/）；第一个作为加速比基准
#   --system-file   路径 ...           额外的系统 JSON 文件（任意位置）
#   --strategy-file 路径 ...           额外的策略 JSON 文件（任意位置）
#   --tasks B1 B5 ...                  只运行指定任务（B1–B6、S18/S20/S22/S24）
#   --no-scaling                       不运行 R-MAT 可扩展性扫描（S18–S24）
#   --out 文件.csv|文件.json            保存逐项结果
#   -h | --help                        显示完整帮助
#
# 示例：
#   bash scripts/run_suite.sh --systems hetero_lab --strategies naive_gpu my_strategy --out results.csv
#   bash scripts/run_suite.sh --systems my_codesign --strategies naive_gpu my_plugin --tasks B4 B5 --no-scaling
#
# 退出码：0 = 全部仿真在 5 分钟内完成；1 = 超时或未完成；2 = 参数错误。仿真失败的组合会在表中显示 ERR 并列出原因。
# 前提：已运行 bash install_deps.sh 与 make build（本脚本不需要 streamlit，只用到 Python 标准库与模拟器）。
# 相对路径（--system-file / --strategy-file / --out）都相对于你运行本脚本时所在的目录。
# =============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$ROOT/.venv/bin/activate" ] && . "$ROOT/.venv/bin/activate"
[ -f "$ROOT/build/libhgcsim.so" ] \
  || { echo "错误：还没有编译模拟器（找不到 build/libhgcsim.so），请先运行 make build" >&2; exit 1; }
export PYTHONPATH="$ROOT/frontend${PYTHONPATH:+:$PYTHONPATH}"
exec python3 -m hgcsim_frontend.suite "$@"
