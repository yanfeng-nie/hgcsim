"""HGCSim Python 绑定（ctypes 调用 C++ 模拟器共享库 libhgcsim.so）。

模拟器的全部功能都在 C++ 中实现；本模块只负责把 JSON 请求交给 C++ 并取回 JSON 结果。
"""
from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
from typing import Any, Dict, Optional

ROOT = Path(__file__).resolve().parents[2]
CONFIG_DIR = ROOT / "configs"
_LIB = None


def _find_library() -> Path:
    env = os.environ.get("HGCSIM_LIB")
    cands = [Path(env)] if env else []
    cands += [ROOT / "build" / "libhgcsim.so", ROOT / "libhgcsim.so"]
    for c in cands:
        if c.exists():
            return c
    raise FileNotFoundError(
        "找不到 libhgcsim.so，请先在仓库根目录编译 C++ 模拟器：\n"
        "  cd {0} && make build\n"
        "或设置环境变量 HGCSIM_LIB 指向该文件".format(ROOT)
    )


def lib() -> ctypes.CDLL:
    global _LIB
    if _LIB is None:
        L = ctypes.CDLL(str(_find_library()))
        for name in ("hgc_catalog", "hgc_build_info"):
            getattr(L, name).restype = ctypes.c_void_p
            getattr(L, name).argtypes = []
        for name in ("hgc_validate_system", "hgc_characterize", "hgc_plan", "hgc_simulate"):
            getattr(L, name).restype = ctypes.c_void_p
            getattr(L, name).argtypes = [ctypes.c_char_p]
        L.hgc_free.argtypes = [ctypes.c_void_p]
        L.hgc_free.restype = None
        L.hgc_version.restype = ctypes.c_char_p
        _LIB = L
    return _LIB


def _call(fn, payload: Optional[Any] = None) -> Dict[str, Any]:
    L = lib()
    if payload is None:
        ptr = fn()
    else:
        ptr = fn(json.dumps(payload, ensure_ascii=False).encode("utf-8"))
    try:
        raw = ctypes.cast(ptr, ctypes.c_char_p).value.decode("utf-8", errors="replace")
    finally:
        L.hgc_free(ptr)
    return json.loads(raw)


def version() -> str:
    return lib().hgc_version().decode()


def build_info() -> Dict[str, Any]:
    """当前加载的 build 信息（名称、时间、包含的调度插件、保留名等）。"""
    return _call(lib().hgc_build_info)


def catalog() -> Dict[str, Any]:
    return _call(lib().hgc_catalog)


def validate_system(system: Dict[str, Any]) -> Dict[str, Any]:
    return _call(lib().hgc_validate_system, system)


def characterize(workload: Dict[str, Any]) -> Dict[str, Any]:
    return _call(lib().hgc_characterize, workload)


def plan(system: Dict[str, Any], workload: Dict[str, Any], strategy: Dict[str, Any]) -> Dict[str, Any]:
    return _call(lib().hgc_plan, {"system": system, "workload": workload, "strategy": resolve_strategy(strategy)})


def simulate(system: Dict[str, Any], workload: Dict[str, Any], strategy: Dict[str, Any],
             options: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    req = {"system": system, "workload": workload, "strategy": resolve_strategy(strategy),
           "options": options or {}}
    return _call(lib().hgc_simulate, req)


def resolve_strategy(strategy: Dict[str, Any]) -> Dict[str, Any]:
    """把策略中的插件相对路径解析为绝对路径（相对仓库根目录）。"""
    s = dict(strategy)
    p = s.get("plugin")
    if p:
        pp = Path(p)
        if not pp.is_absolute():
            pp = ROOT / pp
        s["plugin"] = str(pp)
    return s


def load_json(path: os.PathLike) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def list_configs(kind: str) -> Dict[str, Path]:
    d = CONFIG_DIR / kind
    return {p.stem: p for p in sorted(d.glob("*.json"))}
