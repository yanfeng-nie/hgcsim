"""前端冒烟测试（需要 streamlit>=1.28 的 AppTest）。

运行：  python3 tests/test_frontend.py
覆盖：所有页面、build 名显示、载入/编辑/保存/删除系统与策略配置、名称冲突检查、
      每个组件的参数表单、运行 C++ 插件策略、结果分析、6 个系统配置同时对比、基线套件。
测试期间“我的配置”目录指向临时目录，不会改动仓库中的 configs/user/。
"""
import os
import sys
import tempfile
from pathlib import Path

TMP_USER = tempfile.mkdtemp(prefix="hgcsim-user-")
os.environ["HGCSIM_USER_CONFIG_DIR"] = TMP_USER

from streamlit.testing.v1 import AppTest  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
APP = str(ROOT / "frontend" / "app.py")
sys.path.insert(0, str(ROOT / "frontend"))
from hgcsim_frontend import buildinfo  # noqa: E402

PLUGIN_STRATEGY = "example_plugin"
FAILED = []


def new():
    at = AppTest.from_file(APP, default_timeout=180)
    at.run()
    return at


def check(at, tag):
    if at.exception:
        FAILED.append(tag)
        print(f"[FAIL] {tag}: {[e.value for e in at.exception]}")
    else:
        print(f"[ OK ] {tag}")


def btn(at, label):
    return next(b for b in at.button if b.label == label)


def sel(at, label):
    return next(s for s in at.selectbox if s.label == label)


def goto(at, page):
    at.sidebar.radio[0].set_value(page).run()


def scenario_pages():
    at = new()
    check(at, "概览页")
    side = " ".join(m.value for m in at.sidebar.markdown)
    name = buildinfo.info()["name"]
    assert f"`{name}`" in side, side
    print(f"       侧边栏显示当前 build：{name}")
    for p in ["② 系统配置", "③ 任务与策略", "④ 结果分析", "⑤ 多配置对比", "⑥ 基线测试套件", "⑦ 参数参考"]:
        goto(at, p)
        check(at, p)


def scenario_edit_and_save():
    at = new()
    goto(at, "② 系统配置")
    sel(at, "选择系统配置").set_value("hetero_lab（内置）").run()
    btn(at, "载入（覆盖当前编辑）").click().run()
    check(at, "载入 hetero_lab")
    sel(at, "选择组件").set_value("设备 gpu0").run()
    next(n for n in at.number_input if n.label.startswith("sms")).set_value(100)
    btn(at, "应用修改").click().run()
    check(at, "编辑 GPU 参数")
    assert at.session_state["system"]["devices"][0]["params"]["sms"] == 100
    # 与内置配置重名：拒绝
    next(t for t in at.text_input if t.label.startswith("配置名")).set_value("hetero_lab")
    btn(at, "💾 保存到我的配置").click().run()
    assert any("重名" in e.value for e in at.error), "应拒绝与内置配置重名"
    print("[ OK ] 拒绝与内置配置重名")
    # 正常保存
    next(t for t in at.text_input if t.label.startswith("配置名")).set_value("my_lab")
    btn(at, "💾 保存到我的配置").click().run()
    check(at, "保存为我的系统配置")
    assert (Path(TMP_USER) / "systems" / "my_lab.json").exists()
    assert any("已保存" in m.value for m in at.success), "保存后应提示已保存"
    assert "my_lab（我的）" in sel(at, "选择系统配置").options
    # 本地手工新建一个配置文件，刷新后出现在列表中
    import json
    obj = json.loads((Path(TMP_USER) / "systems" / "my_lab.json").read_text(encoding="utf-8"))
    obj["name"] = "my_lab_local"
    (Path(TMP_USER) / "systems" / "my_lab_local.json").write_text(json.dumps(obj), encoding="utf-8")
    btn(at, "刷新列表").click().run()
    assert "my_lab_local（我的）" in sel(at, "选择系统配置").options
    print("[ OK ] 本地新建的配置文件刷新后可见")
    # 删除
    btn(at, "🗑 删除我的配置 my_lab").click().run()
    check(at, "删除我的系统配置")
    assert not (Path(TMP_USER) / "systems" / "my_lab.json").exists()


def scenario_add_device():
    at = new()
    goto(at, "② 系统配置")
    n0 = len(at.session_state["system"]["devices"])
    sel(at, "模板").set_value(next(o for o in sel(at, "模板").options if o.startswith("cim_mram"))).run()
    btn(at, "添加").click().run()
    check(at, "添加 MRAM 存算一体设备")
    assert len(at.session_state["system"]["devices"]) == n0 + 1


def scenario_every_component():
    at = new()
    goto(at, "② 系统配置")
    for sysname in list(sel(at, "选择系统配置").options):
        sel(at, "选择系统配置").set_value(sysname).run()
        btn(at, "载入（覆盖当前编辑）").click().run()
        comps = list(sel(at, "选择组件").options)
        for comp in comps:
            sel(at, "选择组件").set_value(comp).run()
            if at.exception or not any(b.label == "应用修改" for b in at.button):
                FAILED.append(f"{sysname}/{comp}")
                print(f"[FAIL] {sysname} / {comp}: {[e.value for e in at.exception]}")
        print(f"[ OK ] {sysname}：{len(comps)} 个组件的参数表单均可编辑")


def scenario_strategy_and_run():
    at = new()
    goto(at, "② 系统配置")
    sel(at, "选择系统配置").set_value("hetero_lab（内置）").run()
    btn(at, "载入（覆盖当前编辑）").click().run()
    goto(at, "③ 任务与策略")
    sel(at, "选择策略").set_value(f"{PLUGIN_STRATEGY}（内置）").run()
    btn(at, "载入").click().run()
    tk = sel(at, "任务")
    tk.set_value(tk.options[4]).run()
    btn(at, "▶ 运行仿真").click().run()
    check(at, f"运行 B5 + C++ 插件策略 {PLUGIN_STRATEGY}")
    assert at.session_state["history"][-1]["result"]["status"] == "ok"
    btn(at, "仅查看规划结果").click().run()
    check(at, "查看规划")
    next(t for t in at.text_input if t.label.startswith("配置名")).set_value("my_strategy")
    btn(at, "💾 保存到我的配置").click().run()
    check(at, "保存为我的策略")
    assert (Path(TMP_USER) / "strategies" / "my_strategy.json").exists()
    goto(at, "④ 结果分析")
    check(at, "结果分析页")


def scenario_compare_six():
    import json
    # 准备两个“我的系统配置”，凑满 6 个
    d = Path(TMP_USER) / "systems"
    d.mkdir(parents=True, exist_ok=True)
    base = json.loads((ROOT / "configs" / "systems" / "hetero_lab.json").read_text(encoding="utf-8"))
    for n in ("cmp_a", "cmp_b"):
        base["name"] = n
        (d / f"{n}.json").write_text(json.dumps(base), encoding="utf-8")
    at = new()
    goto(at, "⑤ 多配置对比")
    ms = next(m for m in at.multiselect if m.label.startswith("系统配置"))
    pick = [o for o in ms.options if o.startswith("当前编辑中的系统")] + \
        ["default_pc（内置）", "hetero_lab（内置）", "codesign_budget（内置）", "cmp_a（我的）", "cmp_b（我的）"]
    ms.set_value(pick).run()
    sel(at, "策略（所有系统使用同一策略）").set_value("naive_cpu（内置）").run()
    btn(at, "▶ 运行对比").click().run()
    check(at, "6 个系统配置同时对比")
    ids = at.session_state["compare_ids"]
    assert len(ids) == 6, ids
    assert all(at.session_state["history"][i]["result"]["status"] == "ok" for i in ids)
    print("       6 个运行全部成功，对比视图已渲染")


def scenario_suite():
    at = new()
    goto(at, "⑥ 基线测试套件")
    btn(at, "▶ 运行套件").click().run()
    check(at, "基线测试套件")
    res = at.session_state["suite_result"]
    assert res["within_limit"], res["wall_s"]
    print(f"       套件 {res['completed']} 次仿真，墙钟 {res['wall_s']:.2f} s")


if __name__ == "__main__":
    for f in [scenario_pages, scenario_edit_and_save, scenario_add_device, scenario_every_component,
              scenario_strategy_and_run, scenario_compare_six, scenario_suite]:
        try:
            f()
        except Exception as ex:  # noqa: BLE001
            FAILED.append(f.__name__)
            print(f"[FAIL] {f.__name__}: {ex!r}")
    print("前端测试：" + ("全部通过" if not FAILED else f"失败 {FAILED}"))
    sys.exit(1 if FAILED else 0)
