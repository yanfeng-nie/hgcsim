"""当前 build 信息，以及“源码在 build 之后是否又被修改”的检测。

指纹算法与 cmake/fingerprint.cmake 完全一致：
  SHA256( 按相对路径排序的 "相对路径:文件SHA256\\n" 拼接 )
"""
from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any, Dict, List

from . import core


def _fingerprint(files: List[Path]) -> str:
    acc = ""
    for f in sorted(files, key=lambda p: p.relative_to(core.ROOT).as_posix()):
        rel = f.relative_to(core.ROOT).as_posix()
        acc += f"{rel}:{hashlib.sha256(f.read_bytes()).hexdigest()}\n"
    return hashlib.sha256(acc.encode("utf-8")).hexdigest()


def source_fingerprint() -> str:
    r = core.ROOT
    files = [r / "CMakeLists.txt"] if (r / "CMakeLists.txt").exists() else []
    files += list((r / "include" / "hgcsim").glob("*.hpp")) + list((r / "include" / "hgcsim").glob("*.h"))
    files += list((r / "src").glob("*.cpp")) + list((r / "plugins").glob("*.cpp"))
    return _fingerprint([f for f in files if f.is_file()])


def student_fingerprint() -> str:
    return _fingerprint([f for f in (core.ROOT / "plugins" / "student").glob("*.cpp") if f.is_file()])


def student_plugin_sources() -> List[str]:
    return sorted(p.stem for p in (core.ROOT / "plugins" / "student").glob("*.cpp"))


def info() -> Dict[str, Any]:
    """返回已加载 build 的信息，并附加 stale（源码在 build 之后被修改）标志。"""
    b = core.build_info()
    try:
        b["stale_source"] = b.get("source_fingerprint") != source_fingerprint()
        b["stale_plugins"] = b.get("student_fingerprint") != student_fingerprint()
    except OSError:
        b["stale_source"] = b["stale_plugins"] = False
    b["stale"] = b["stale_source"] or b["stale_plugins"]
    return b


def main() -> int:
    """命令行查看当前 build（make info 调用）：python3 -m hgcsim_frontend.buildinfo"""
    try:
        b = info()
    except (OSError, FileNotFoundError) as ex:
        print(f"无法加载 build/libhgcsim.so：{ex}")
        return 1
    print(f"当前 build 名：{b['name']}")
    print(f"版本        ：{b['version']}")
    print(f"编译时间    ：{b['time']}")
    print(f"源码状态    ：{'已修改（相对发布时的原始源码）' if b.get('modified') == 'ON' else '原始源码'}")
    print(f"调度插件    ：{', '.join(b['plugins'])}  （位于 build/plugins/lib<插件名>.so）")
    print(f"保留 build 名：{', '.join(b['reserved_names'])}  （自定义 build 名不能与之重复，不区分大小写）")
    if b["stale"]:
        what = []
        if b["stale_source"]:
            what.append("模拟器源码")
        if b["stale_plugins"]:
            what.append("plugins/student/ 下的插件")
        print(f"[注意] {'、'.join(what)}在本次 build 之后又被修改过，请重新运行：make build BUILD_NAME=<build名>")
    else:
        print("源码与插件自本次 build 之后未被修改。")
    return 0


if __name__ == "__main__":
    import sys

    sys.exit(main())
