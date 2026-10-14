#!/usr/bin/env python3
"""把前端定义的基线任务导出为 configs/workloads/*.json（供命令行 hgcsim_cli 使用）。"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "frontend"))
from hgcsim_frontend import tasks  # noqa: E402

out = ROOT / "configs" / "workloads"
out.mkdir(parents=True, exist_ok=True)
for b in tasks.BASELINES:
    (out / f"{b['id']}.json").write_text(json.dumps(b["workload"], ensure_ascii=False, indent=2), encoding="utf-8")
for sc in tasks.SCALING_SCALES:
    (out / f"S{sc}.json").write_text(json.dumps(tasks.rmat_workload(sc), ensure_ascii=False, indent=2), encoding="utf-8")
print("已导出到", out)
