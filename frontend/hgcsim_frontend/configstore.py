"""配置的加载位置与保存。

  内置配置（只读，随代码包发布）：
      configs/systems/*.json          系统（硬件）配置
      configs/strategies/*.json       调度策略
  我的配置（答题人创建，前端“保存”按钮与本地手工编辑写的都是这里）：
      configs/user/systems/*.json
      configs/user/strategies/*.json
  可用环境变量 HGCSIM_USER_CONFIG_DIR 把“我的配置”目录改到别处。

配置名就是文件名（不含 .json）。我的配置不能与内置配置重名（不区分大小写）。
"""
from __future__ import annotations

import json
import os
import re
from collections import OrderedDict
from pathlib import Path
from typing import Any, Dict, Optional

from . import core

KINDS = {"systems": "系统配置", "strategies": "调度策略"}
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,39}$")
NAME_RULE = "以字母或数字开头，只含字母、数字、下划线、短横线，长度 1~40"


def builtin_dir(kind: str) -> Path:
    return core.CONFIG_DIR / kind


def user_root() -> Path:
    env = os.environ.get("HGCSIM_USER_CONFIG_DIR")
    return Path(env).expanduser().resolve() if env else core.CONFIG_DIR / "user"


def user_dir(kind: str) -> Path:
    return user_root() / kind


def _extra_builtin(kind: str) -> Dict[str, Path]:
    extra: Dict[str, Path] = {}
    return extra


def entries(kind: str) -> "OrderedDict[str, Dict[str, Any]]":
    """返回 名称 -> {path, source('builtin'|'user'), label}；每次调用都重新扫描磁盘。"""
    out: "OrderedDict[str, Dict[str, Any]]" = OrderedDict()
    for p in sorted(builtin_dir(kind).glob("*.json")):
        out[p.stem] = {"path": p, "source": "builtin"}
    for n, p in _extra_builtin(kind).items():
        out[n] = {"path": p, "source": "builtin"}
    lower = {k.lower() for k in out}
    ud = user_dir(kind)
    if ud.is_dir():
        for p in sorted(ud.glob("*.json")):
            name = p.stem if p.stem.lower() not in lower else "user:" + p.stem
            out[name] = {"path": p, "source": "user"}
    for n, e in out.items():
        e["label"] = f"{n}（内置）" if e["source"] == "builtin" else f"{n}（我的）"
    return out


def paths(kind: str) -> Dict[str, Path]:
    return {n: e["path"] for n, e in entries(kind).items()}


def load(kind: str, name: str) -> Dict[str, Any]:
    e = entries(kind)
    if name not in e:
        raise KeyError(f"找不到{KINDS[kind]} '{name}'。可选：{list(e)}")
    return core.load_json(e[name]["path"])


def check_name(kind: str, name: str) -> Optional[str]:
    """返回错误信息；None 表示名称可用（可能覆盖自己已有的同名配置）。"""
    if not NAME_RE.match(name or ""):
        return f"名称不合法：{NAME_RULE}"
    builtin = {n.lower() for n, e in entries(kind).items() if e["source"] == "builtin"}
    if name.lower() in builtin:
        return f"'{name}' 与内置{KINDS[kind]}重名，请换一个名称"
    return None


def user_exists(kind: str, name: str) -> bool:
    return (user_dir(kind) / f"{name}.json").exists()


def save(kind: str, name: str, obj: Dict[str, Any], overwrite: bool = False) -> Path:
    err = check_name(kind, name)
    if err:
        raise ValueError(err)
    p = user_dir(kind) / f"{name}.json"
    if p.exists() and not overwrite:
        raise FileExistsError(f"我的{KINDS[kind]} '{name}' 已存在，勾选“覆盖”后再保存")
    p.parent.mkdir(parents=True, exist_ok=True)
    data = dict(obj)
    data["name"] = name
    tmp = p.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    tmp.replace(p)
    return p


def delete(kind: str, name: str) -> Path:
    e = entries(kind).get(name)
    if not e or e["source"] != "user":
        raise ValueError("只能删除“我的”配置，内置配置不能删除")
    e["path"].unlink()
    return e["path"]


def rel(p: Path) -> str:
    try:
        return p.resolve().relative_to(core.ROOT).as_posix()
    except ValueError:
        return str(p)
