"""基线测试套件：批量运行全部基线任务，统计墙钟时间（要求 < 5 分钟）。

通常通过 bash scripts/run_suite.sh 调用（会自动激活 .venv 并设置 PYTHONPATH），参数完全相同：
  bash scripts/run_suite.sh                         # 默认组合
  bash scripts/run_suite.sh --list                  # 列出可用的系统、策略与任务
  bash scripts/run_suite.sh --systems default_pc hetero_lab --strategies naive_gpu my_strategy
  bash scripts/run_suite.sh --strategy-file my.json --out results.json
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from . import buildinfo, configstore, core, tasks

WALL_LIMIT_S = 300.0
DEFAULT_SYSTEMS = ["default_pc", "hetero_lab"]
DEFAULT_STRATEGIES = ["naive_cpu", "naive_gpu", "hetero_template"]


def available_systems() -> Dict[str, Path]:
    """内置 + 我的系统配置（configs/systems/、configs/user/systems/）。"""
    return configstore.paths("systems")


def available_strategies() -> Dict[str, Path]:
    """内置 + 我的调度策略（configs/strategies/、configs/user/strategies/）。"""
    return configstore.paths("strategies")


def run_one(system: Dict[str, Any], workload: Dict[str, Any], strategy: Dict[str, Any]) -> Dict[str, Any]:
    t0 = time.time()
    r = core.simulate(system, workload, strategy)
    r["_wall"] = time.time() - t0
    return r


def run_suite(systems: Dict[str, Dict[str, Any]], strategies: Dict[str, Dict[str, Any]],
              include_scaling: bool = True, progress: Optional[Callable[[float, str], None]] = None,
              task_ids: Optional[List[str]] = None) -> Dict[str, Any]:
    jobs = []
    for b in tasks.BASELINES:
        if task_ids and b["id"] not in task_ids:
            continue
        for sn in systems:
            for stn in strategies:
                jobs.append((b["id"], b["workload"], sn, stn))
    if include_scaling:
        for sc in tasks.SCALING_SCALES:
            wl = tasks.rmat_workload(sc)
            for sn in systems:
                for stn in strategies:
                    jobs.append((f"S{sc}", wl, sn, stn))
    rows = []
    t_start = time.time()
    for i, (tid, wl, sn, stn) in enumerate(jobs):
        if progress:
            progress(i / max(1, len(jobs)), f"{tid} · {sn} · {stn}")
        r = run_one(systems[sn], wl, strategies[stn])
        row = {"task": tid, "workload": wl["name"], "system": sn, "strategy": stn, "status": r["status"],
               "makespan_s": r.get("makespan_s"), "energy_j": r.get("energy", {}).get("total_j"),
               "chunks": r.get("graph", {}).get("chunks"), "transfer_GB": r.get("transfers", {}).get("total_GB"),
               "wall_s": r["_wall"], "error": "; ".join(r.get("errors", []))[:300]}
        rows.append(row)
        if time.time() - t_start > WALL_LIMIT_S:
            break
    if progress:
        progress(1.0, "完成")
    wall = time.time() - t_start
    # 相对加速比（同一系统、同一任务，以第一个策略为基准）
    base_st = list(strategies)[0] if strategies else None
    for row in rows:
        base = next((x for x in rows if x["task"] == row["task"] and x["system"] == row["system"]
                     and x["strategy"] == base_st and x["status"] == "ok"), None)
        if base and row["status"] == "ok" and row["makespan_s"]:
            row["speedup_vs_" + base_st] = base["makespan_s"] / row["makespan_s"]
    return {"rows": rows, "wall_s": wall, "jobs": len(jobs), "completed": len(rows),
            "within_limit": wall < WALL_LIMIT_S and len(rows) == len(jobs)}


def _fmt(x: Optional[float]) -> str:
    if x is None:
        return "-"
    if x >= 100:
        return f"{x:.1f}"
    if x >= 1:
        return f"{x:.3f}"
    return f"{x * 1000:.2f}m"


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        prog="bash scripts/run_suite.sh",
        description="HGCSim 基线测试套件：批量运行 B1–B6 与 R-MAT 可扩展性扫描（墙钟上限 300 秒）。"
                    "系统/策略名可以是内置配置，也可以是 configs/user/ 下“我的配置”。",
        epilog="退出码：0 = 全部仿真在上限内完成；1 = 超时或未完成；2 = 参数错误。")
    ap.add_argument("--list", action="store_true", help="列出可用的系统、策略与任务后退出")
    ap.add_argument("--systems", nargs="*", default=None, metavar="名称",
                    help=f"系统配置名（默认：{' '.join(DEFAULT_SYSTEMS)}）")
    ap.add_argument("--strategies", nargs="*", default=None, metavar="名称",
                    help=f"策略名，第一个作为加速比基准（默认：{' '.join(DEFAULT_STRATEGIES)}）")
    ap.add_argument("--system-file", nargs="*", default=[], metavar="路径", help="额外的系统 JSON 文件（名称取文件名）")
    ap.add_argument("--strategy-file", nargs="*", default=[], metavar="路径", help="额外的策略 JSON 文件（名称取文件名）")
    ap.add_argument("--tasks", nargs="*", default=None, metavar="任务",
                    help="只运行指定任务，如 B1 B5 S20（默认：B1–B6 与 S18–S24 全部）")
    ap.add_argument("--no-scaling", action="store_true", help="不运行 R-MAT 可扩展性扫描（S18–S24）")
    ap.add_argument("--out", default=None, metavar="文件", help="逐项结果输出到 .csv 或 .json")
    a = ap.parse_args(argv)

    allsys = available_systems()
    allst = available_strategies()
    if a.list:
        ents = {k: configstore.entries(k) for k in ("systems", "strategies")}
        for kind, title in (("systems", "系统配置"), ("strategies", "调度策略")):
            print(f"{title}：")
            for n, e in ents[kind].items():
                desc = core.load_json(e["path"]).get("description", "")
                print(f"  {n:<20s} {'内置' if e['source'] == 'builtin' else '我的'}  {desc[:70]}")
        print("任务：")
        for b in tasks.BASELINES:
            print(f"  {b['id']:<6s} {b['title']}")
        print("  " + " ".join(f"S{sc}" for sc in tasks.SCALING_SCALES) + "  R-MAT 合成图（节点数 2^scale），--no-scaling 时不运行")
        print(f"默认组合：--systems {' '.join(DEFAULT_SYSTEMS)} --strategies {' '.join(DEFAULT_STRATEGIES)}")
        return 0
    sysnames = a.systems if a.systems is not None else [n for n in DEFAULT_SYSTEMS if n in allsys]
    stnames = a.strategies if a.strategies is not None else [n for n in DEFAULT_STRATEGIES if n in allst]
    for n in sysnames:
        if n not in allsys:
            ap.error(f"未知系统 {n}，可选: {list(allsys)}")
    for n in stnames:
        if n not in allst:
            ap.error(f"未知策略 {n}，可选: {list(allst)}")
    systems = {n: core.load_json(allsys[n]) for n in sysnames}
    for f in a.system_file:
        systems[Path(f).stem] = core.load_json(f)
    strategies = {n: core.load_json(allst[n]) for n in stnames}
    for f in a.strategy_file:
        strategies[Path(f).stem] = core.load_json(f)

    b = buildinfo.info()
    print(f"{core.version()}  |  build: {b['name']}（{b['time']}）  |  系统: {list(systems)}  |  策略: {list(strategies)}")
    if b["stale"]:
        print("[警告] 源码或插件在本次 build 之后被修改过，请重新运行 make build BUILD_NAME=<build名>")
    res = run_suite(systems, strategies, include_scaling=not a.no_scaling, task_ids=a.tasks,
                    progress=lambda p, m: print(f"\r[{p * 100:5.1f}%] {m:60s}", end="", file=sys.stderr))
    print(file=sys.stderr)
    cols = [(s, st) for s in systems for st in strategies]
    head = "任务  " + " ".join(f"{(s[:10] + '/' + st[:10]):>22s}" for s, st in cols)
    print("\n仿真完成时间（秒；m=毫秒）")
    print(head)
    for tid in dict.fromkeys(r["task"] for r in res["rows"]):
        cells = []
        for s, st in cols:
            r = next((x for x in res["rows"] if x["task"] == tid and x["system"] == s and x["strategy"] == st), None)
            cells.append(f"{(_fmt(r['makespan_s']) if r and r['status'] == 'ok' else 'ERR'):>22s}")
        print(f"{tid:5s} " + " ".join(cells))
    errs = [r for r in res["rows"] if r["status"] != "ok"]
    for r in errs:
        print(f"  [错误] {r['task']} {r['system']} {r['strategy']}: {r['error']}")
    base_st = list(strategies)[0]
    if len(strategies) > 1:
        print(f"\nB1–B6 几何平均加速比（相对 {base_st}，只统计两者都成功的任务）")
        for s in systems:
            cells = []
            for st in list(strategies)[1:]:
                sp = [r.get("speedup_vs_" + base_st) for r in res["rows"]
                      if r["system"] == s and r["strategy"] == st and r["task"].startswith("B")]
                sp = [x for x in sp if x]
                g = math.exp(sum(math.log(x) for x in sp) / len(sp)) if sp else None
                cells.append(f"{st}: {g:.2f}×（{len(sp)} 个任务）" if g else f"{st}: -")
            print(f"  {s:<16s} " + "   ".join(cells))
    print(f"\n共 {res['completed']}/{res['jobs']} 次仿真，墙钟时间 {res['wall_s']:.2f} s "
          f"（上限 {WALL_LIMIT_S:.0f} s）：{'通过' if res['within_limit'] else '超时'}")
    if a.out:
        p = Path(a.out)
        if p.suffix == ".csv":
            keys = sorted({k for r in res["rows"] for k in r})
            with open(p, "w", newline="", encoding="utf-8") as f:
                w = csv.DictWriter(f, fieldnames=keys)
                w.writeheader()
                w.writerows(res["rows"])
        else:
            p.write_text(json.dumps(res, ensure_ascii=False, indent=1), encoding="utf-8")
        print(f"结果已写入 {p}")
    return 0 if res["within_limit"] else 1


if __name__ == "__main__":
    sys.exit(main())
