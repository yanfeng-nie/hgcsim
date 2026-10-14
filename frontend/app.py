"""HGCSim 网页前端（Streamlit）。

启动：  bash scripts/run_frontend.sh      （先 bash install_deps.sh 安装依赖、make build 编译）
模拟器的全部计算都在 C++ 中完成；本前端只负责编辑配置、下发任务、展示结果。

配置加载位置（见 hgcsim_frontend/configstore.py）：
  内置（只读）： configs/systems/、configs/strategies/
  我的配置：     configs/user/systems/、configs/user/strategies/   ← 前端“保存”与本地手工编辑都写这里
"""
from __future__ import annotations

import copy
import json
import sys
import time
from pathlib import Path

import pandas as pd
import streamlit as st

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hgcsim_frontend import buildinfo, configstore, core, suite, tasks, viz  # noqa: E402

st.set_page_config(page_title="HGCSim · 大规模图聚类异构计算模拟器", page_icon="🧮", layout="wide")

_V = tuple(int(x) for x in st.__version__.split(".")[:2])
MAX_COMPARE = 6


_FIG_SEQ = [0]


def show_fig(fig):
    # 每个图表使用唯一 key：内容完全相同的两张图（例如对比同一系统两次）也不会冲突
    _FIG_SEQ[0] += 1
    kw = {"theme": None} if _V >= (1, 16) else {}
    kw["key"] = f"fig-{_FIG_SEQ[0]}"
    try:
        if _V >= (1, 46):
            st.plotly_chart(fig, width="stretch", **kw)
        else:
            st.plotly_chart(fig, use_container_width=True, **kw)
    except TypeError:
        kw.pop("key", None)
        st.plotly_chart(fig, use_container_width=True, **kw)


def show_df(df, height=None):
    kw = {"hide_index": True} if _V >= (1, 23) else {}
    if height:
        kw["height"] = height
    if _V >= (1, 46):
        st.dataframe(df, width="stretch", **kw)
    else:
        st.dataframe(df, use_container_width=True, **kw)


def multiselect_max(label, options, default, max_n, **kw):
    try:
        return st.multiselect(label, options, default=default[:max_n], max_selections=max_n, **kw)
    except TypeError:
        v = st.multiselect(label, options, default=default[:max_n], **kw)
        if len(v) > max_n:
            st.warning(f"最多选择 {max_n} 项，只使用前 {max_n} 项")
        return v[:max_n]


def labeled_select(label, items, index=0, key=None):
    """items: [(取值, 显示文本)]；返回取值。选项直接使用显示文本，保证浏览器与测试环境行为一致。"""
    texts = [t for _, t in items]
    if not texts:
        return None
    t = st.selectbox(label, texts, index=min(max(index, 0), len(texts) - 1), key=key)
    return items[texts.index(t)][0]


def labeled_multiselect(label, items, default_values, max_n=None, key=None):
    texts = [t for _, t in items]
    by_val = {v: t for v, t in items}
    dflt = [by_val[v] for v in default_values if v in by_val]
    if max_n:
        chosen = multiselect_max(label, texts, dflt, max_n, key=key)
    else:
        chosen = st.multiselect(label, texts, default=dflt, key=key)
    return [items[texts.index(t)][0] for t in chosen]


def config_items(kind):
    return [(n, e["label"]) for n, e in configstore.entries(kind).items()]


def rerun():
    if hasattr(st, "rerun"):
        st.rerun()
    else:
        st.experimental_rerun()


def h(text):
    st.markdown(f"##### {text}")


# ---------------------------------------------------------------------------
# 状态
# ---------------------------------------------------------------------------
@st.cache_data(show_spinner=False)
def catalog():
    return core.catalog()


ss = st.session_state


def load_system(name):
    ss.system = configstore.load("systems", name)
    ss.system_name = name
    ss.system_snapshot = json.dumps(ss.system, sort_keys=True)


def load_strategy(name):
    ss.strategy_text = json.dumps(configstore.load("strategies", name), ensure_ascii=False, indent=2)
    ss.strategy_name = name
    ss.strategy_snapshot = ss.strategy_text


if "system" not in ss:
    load_system("default_pc")
if "strategy_text" not in ss:
    load_strategy("naive_gpu")
if "history" not in ss:
    ss.history = []
if "compare_ids" not in ss:
    ss.compare_ids = []


def validated():
    return core.validate_system(ss.system)


def system_dirty():
    return json.dumps(ss.system, sort_keys=True) != ss.get("system_snapshot")


def strategy_dirty():
    return ss.strategy_text != ss.get("strategy_snapshot")


# ---------------------------------------------------------------------------
# 侧边栏：build 信息 + 导航
# ---------------------------------------------------------------------------
st.sidebar.title("HGCSim")
st.sidebar.caption("大规模图聚类的异构计算技术 · 体系结构模拟器")
try:
    B = buildinfo.info()
except Exception as ex:  # noqa: BLE001
    st.sidebar.error(str(ex))
    st.error("C++ 模拟器库无法加载：\n\n" + str(ex))
    st.stop()
st.sidebar.markdown(f"**当前 build**：`{B['name']}`")
st.sidebar.caption(f"{B['version']} · 编译于 {B['time']}\n\n调度插件：{', '.join(B['plugins'])}")
if B["stale"]:
    st.sidebar.warning("源码或调度插件在本次 build 之后被修改过。请运行\n\n`make build BUILD_NAME=<build名>`\n\n然后停止并重新运行 `bash scripts/run_frontend.sh`。")
PAGES = ["① 概览", "② 系统配置", "③ 任务与策略", "④ 结果分析", "⑤ 多配置对比", "⑥ 基线测试套件", "⑦ 参数参考"]
page = st.sidebar.radio("页面", PAGES)
st.sidebar.markdown("---")
st.sidebar.markdown(f"**当前系统**：`{ss.system_name}`" + ("（有未保存修改）" if system_dirty() else ""))
st.sidebar.markdown(f"**当前策略**：`{ss.strategy_name}`" + ("（有未保存修改）" if strategy_dirty() else ""))
st.sidebar.markdown(f"**运行记录**：{len(ss.history)} 条")


def config_locations(kind):
    ents = configstore.entries(kind)
    nb = sum(1 for e in ents.values() if e["source"] == "builtin")
    nu = len(ents) - nb
    st.caption(
        f"配置加载位置 · 内置{configstore.KINDS[kind]}（只读，{nb} 个）：`{configstore.rel(configstore.builtin_dir(kind))}/`"
        f"　·　我的{configstore.KINDS[kind]}（{nu} 个）：`{configstore.rel(configstore.user_dir(kind))}/`"
        f"　——在本地新建或编辑该目录下的 JSON 文件后，点“刷新列表”即可看到。")


def save_panel(kind, obj, current_name, key):
    """保存到“我的配置”目录的表单。返回保存后的名称或 None。"""
    ents = configstore.entries(kind)
    is_user = ents.get(current_name, {}).get("source") == "user"
    default_name = current_name if is_user else f"{current_name}_mine"
    with st.form(key):
        c1, c2, c3 = st.columns([3, 1.2, 1.2])
        name = c1.text_input("配置名（即文件名）", value=default_name, help=configstore.NAME_RULE)
        overwrite = c2.checkbox("覆盖同名配置", value=False)
        c3.markdown("&nbsp;")
        ok = c3.form_submit_button("💾 保存到我的配置")
    if ok:
        try:
            p = configstore.save(kind, name.strip(), obj, overwrite=overwrite)
            ss.flash = f"已保存到我的{configstore.KINDS[kind]}：`{configstore.rel(p)}`"
            return name.strip()
        except (ValueError, FileExistsError) as ex:
            st.error(str(ex))
    return None


def show_flash():
    msg = ss.pop("flash", None) if "flash" in ss else None
    if msg:
        st.success(msg)


# ---------------------------------------------------------------------------
# ① 概览
# ---------------------------------------------------------------------------
def page_overview():
    st.title("大规模图聚类的异构计算技术")
    st.markdown(
        """
**题目**：如何针对大规模图聚类不同阶段在计算密度、内存访问模式和数据搬运需求上的差异，设计高效的异构计算
任务调度与数据管理机制，以降低计算与存储资源之间的不匹配，提升执行效率与可扩展性？

本模拟器用**离散事件仿真 + 扩展 Roofline + 流级网络模型**刻画一台完整计算机：CPU / GPU / NPU / TPU / FPGA /
近内存计算 / 可计算存储 / RRAM·MRAM 存算一体，HBM / GDDR / DRAM / SCM / Flash 存储层次，以及 PCIe / CXL / DDR / NVLink 互连。
不做真实数值计算，所有工作量由数据集参数解析推导，因此全套基线测试可在数秒内完成。
"""
    )
    c1, c2, c3 = st.columns(3)
    c1.markdown("**答题方式 1：JSON 策略**\n\n在“任务与策略”页编辑设备映射、精度、数据放置、分块、流水线，并保存到“我的策略”。")
    c2.markdown("**答题方式 2：C++ 调度插件**\n\n在 `plugins/student/` 实现 `IScheduler`，用 `make build BUILD_NAME=<build名>` 编译。")
    c3.markdown("**协同设计赛道**\n\n从 `codesign_budget` 出发，在预算内（功耗/成本）自选硬件并保存为“我的系统”。")
    h("配置文件放在哪里")
    show_df(pd.DataFrame([
        {"类型": "系统配置", "内置（只读）": f"{configstore.rel(configstore.builtin_dir('systems'))}/",
         "我的配置（前端保存 / 本地编辑）": f"{configstore.rel(configstore.user_dir('systems'))}/"},
        {"类型": "调度策略", "内置（只读）": f"{configstore.rel(configstore.builtin_dir('strategies'))}/",
         "我的配置（前端保存 / 本地编辑）": f"{configstore.rel(configstore.user_dir('strategies'))}/"},
        {"类型": "C++ 调度插件", "内置（只读）": "plugins/example_scheduler.cpp（模板）",
         "我的配置（前端保存 / 本地编辑）": "plugins/student/*.cpp（编译后为 build/plugins/lib<文件名>.so）"},
    ]))
    h("流水线与负载特征")
    st.markdown(
        """
| 阶段 | 典型算子 | 负载特征 | 可调映射 |
|---|---|---|---|
| 数据加载 | 从 SSD/CSD 读特征 X 与边表 G | I/O 带宽受限 | `dataset_location`、`placement.features/graph` |
| 特征提取 | MLP/自编码器 GEMM（可含训练 epoch） | **计算密集** | `stages.feature_extraction` |
| 图构建 | 边排序 + 度统计 + CSR 归一化 / kNN 距离 GEMM + Top-k | **内存密集** | `stages.graph_construction` |
| 聚类迭代 | SpMM 图传播（随机 gather）+ 分配 GEMM + 全局归约 | **不规则访存 + 迭代同步** | `stages.clustering.propagation/assignment` |
"""
    )
    h("基线任务")
    rows = []
    for b in tasks.BASELINES:
        ch = core.characterize(b["workload"])
        d = b["workload"]["dataset"]
        rows.append({"任务": b["id"], "名称": b["title"], "节点": f"{d['nodes']:,}", "边": f"{d['edges']:,}",
                     "特征维": d["feature_dim"], "簇数": d["clusters"], "特征 GB": round(ch["feature_GB"], 2),
                     "CSR GB": round(ch["csr_GB"], 2),
                     "特征提取 GFLOP": round(ch["stages"]["feature_extraction"]["GFLOP"], 1),
                     "图构建 GFLOP": round(ch["stages"]["graph_construction"]["GFLOP"], 1),
                     "聚类 GFLOP": round(ch["stages"]["clustering"]["GFLOP"], 1),
                     "聚类随机访问 (百万次)": round(ch["stages"]["clustering"]["random_accesses_M"], 1)})
    show_df(pd.DataFrame(rows))
    for b in tasks.BASELINES:
        st.markdown(f"- **{b['id']} {b['title']}**：{b['description']}")


# ---------------------------------------------------------------------------
# ② 系统配置
# ---------------------------------------------------------------------------
def param_form(key, specs, current, title):
    """渲染参数编辑表单，返回新的参数字典（仅在提交时）。"""
    with st.form(key):
        st.markdown(f"**{title}**")
        vals = {}
        cols = st.columns(3)
        for i, sp in enumerate(specs):
            col = cols[i % 3]
            v = current.get(sp["name"], sp["default"])
            try:
                v = min(max(float(v), float(sp["min"])), float(sp["max"]))
            except (TypeError, ValueError):
                v = sp["default"]
            label = f"{sp['name']} ({sp['unit']})" if sp["unit"] else sp["name"]
            helptext = f"{sp['desc']}｜范围 [{sp['min']}, {sp['max']}]，默认 {sp['default']}"
            if sp.get("choices"):
                ch = sp["choices"]
                idx = min(range(len(ch)), key=lambda j: abs(ch[j] - v))
                vals[sp["name"]] = col.selectbox(label, ch, index=idx, help=helptext, key=f"{key}-{sp['name']}")
            elif sp["integer"]:
                vals[sp["name"]] = col.number_input(label, min_value=int(sp["min"]), max_value=int(sp["max"]),
                                                    value=int(round(v)), step=1, help=helptext, key=f"{key}-{sp['name']}")
            else:
                vals[sp["name"]] = col.number_input(label, min_value=float(sp["min"]), max_value=float(sp["max"]),
                                                    value=float(v), help=helptext, format="%.6g", key=f"{key}-{sp['name']}")
        ok = st.form_submit_button("应用修改")
    return vals if ok else None


def page_system():
    st.title("系统配置")
    show_flash()
    cat = catalog()
    config_locations("systems")
    ents = configstore.entries("systems")
    names = list(ents)
    c1, c2, c3 = st.columns([3, 1.3, 1])
    cur = ss.system_name if ss.system_name in names else names[0]
    with c1:
        sel = labeled_select("选择系统配置", config_items("systems"), names.index(cur))
    c2.markdown("&nbsp;")
    if c2.button("载入（覆盖当前编辑）"):
        load_system(sel)
        rerun()
    c3.markdown("&nbsp;")
    if c3.button("刷新列表"):
        rerun()
    with st.expander("从 JSON 文件上传 / 下载当前配置"):
        up = st.file_uploader("上传系统配置 JSON", type=["json"])
        if up is not None and st.button("使用上传的系统配置"):
            ss.system = json.loads(up.getvalue().decode("utf-8"))
            ss.system_name = ss.system.get("name", "uploaded")
            ss.system_snapshot = ""
            rerun()
        st.download_button("下载当前系统 JSON", json.dumps(ss.system, ensure_ascii=False, indent=2),
                           file_name=f"{ss.system_name}.json", mime="application/json")

    src = ents.get(ss.system_name, {}).get("source")
    where = "内置配置" if src == "builtin" else ("我的配置" if src == "user" else "未保存的配置")
    state = "　**● 有未保存的修改**" if system_dirty() else "　已与文件一致"
    st.markdown(f"正在编辑：**{ss.system_name}**（{where}）{state}")
    v = validated()
    sysd = v.get("system")
    if ss.system.get("description"):
        st.caption(ss.system["description"])
    if v["ok"]:
        st.success("配置合法（所有参数在上下界内，关系约束满足）")
    else:
        st.error("配置不合法：\n\n" + "\n".join("- " + e for e in v["errors"]))

    h("保存为我的系统配置")
    saved = save_panel("systems", ss.system, ss.system_name, "save-system")
    if saved:
        ss.system["name"] = saved
        ss.system_name = saved
        ss.system_snapshot = json.dumps(ss.system, sort_keys=True)
        rerun()
    if src == "user":
        if st.button(f"🗑 删除我的配置 {ss.system_name}"):
            p = configstore.delete("systems", ss.system_name)
            ss.flash = f"已删除 `{configstore.rel(p)}`（当前编辑内容仍保留，可重新保存）"
            ss.system_snapshot = ""
            rerun()
    if not sysd:
        return
    tot = sysd["totals"]
    m1, m2, m3, m4 = st.columns(4)
    m1.metric("计算/存储设备数", len(sysd["devices"]))
    m2.metric("估算总成本", f"${tot['cost_usd']:,.0f}", help="简化成本模型，仅用于预算约束，见 docs/MODEL.md")
    m3.metric("估算峰值功耗", f"{tot['tdp_w']:,.0f} W")
    if tot["budget_enabled"]:
        okb = tot["tdp_w"] <= tot["power_budget_w"] and tot["cost_usd"] <= tot["cost_budget_usd"]
        m4.metric("预算", "满足" if okb else "超出")
        st.caption(f"预算上限：功耗 ≤ {tot['power_budget_w']:,.0f} W，成本 ≤ ${tot['cost_budget_usd']:,.0f}")
    else:
        m4.metric("预算约束", "未启用")

    t1, t2, t3, t4 = st.tabs(["拓扑", "设备 / 存储 / 链路", "编辑参数", "增删设备与预算"])
    with t1:
        show_fig(viz.topology(sysd))
    with t2:
        rows = []
        for d in sysd["devices"]:
            pk = d.get("peak", {})
            mem = d.get("memory", {})
            rows.append({"设备": d["id"], "类型": d["type"],
                         "FP32 TFLOPS": round(pk.get("fp32", {}).get("matrix_TFLOPS", 0), 2),
                         "FP16 矩阵 TFLOPS": round(pk.get("fp16", {}).get("matrix_TFLOPS", 0), 1),
                         "INT8 矩阵 TOPS": round(pk.get("int8", {}).get("matrix_TFLOPS", 0), 1),
                         "本地存储": f"{mem.get('kind', '-')} {mem.get('capacity_GB', 0):.0f}GB",
                         "存储带宽 GB/s": round(mem.get("seq_read_GBps", 0), 1),
                         "功耗 W": round(d["tdp_w"]), "成本 $": round(d["cost_usd"])})
        show_df(pd.DataFrame(rows))
        show_df(pd.DataFrame([{"存储": m["id"], "类型": m["kind"], "容量 GB": round(m["capacity_GB"], 1),
                               "峰值 GB/s": round(m["peak_bw_GBps"], 1), "顺序读 GB/s": round(m["seq_read_GBps"], 1),
                               "延迟 ns": round(m["latency_ns"]), "粒度 B": m["granularity_B"],
                               "字节寻址": m["byte_addressable"]} for m in sysd["memories"]]))
        show_df(pd.DataFrame([{"链路": l["id"], "类型": l["kind"], "原始 GB/s": round(l["raw_GBps"], 2),
                               "有效 GB/s": round(l["effective_GBps"], 2), "延迟 ns": round(l["latency_ns"]),
                               "全双工": l["full_duplex"], "访存语义": l["load_store"]}
                              for l in sysd["links"] if l["kind"] != "MEDIA"]))
    with t3:
        edit_components(cat)
    with t4:
        add_remove_budget()


def edit_components(cat):
    sysj = ss.system
    comps = ["主机 CPU", "主机内存 " + sysj.get("host", {}).get("memory", {}).get("id", "dram0")]
    for d in sysj.get("devices", []):
        comps += [f"设备 {d['id']}", f"设备存储 {d['id']}", f"挂载链路 {d['id']}"]
    for s in sysj.get("switches", []):
        comps += [f"交换机 {s['id']}", f"交换机链路 {s['id']}"]
    comp = st.selectbox("选择组件", comps)
    kind, cid = comp.split(" ", 1) if " " in comp else (comp, "")
    new = None
    if comp == "主机 CPU":
        hj = sysj.setdefault("host", {})
        new = param_form("f-host", cat["devices"]["CPU"]["params"], hj.get("params", {}), "主机 CPU 参数")
        if new is not None:
            hj["params"] = new
    elif kind == "主机内存":
        mm = sysj.setdefault("host", {}).setdefault("memory", {"id": "dram0", "type": "DRAM"})
        new = param_form("f-hmem", cat["memories"][mm.get("type", "DRAM")], mm.get("params", {}), "主机内存参数")
        if new is not None:
            mm["params"] = new
    elif kind in ("设备", "设备存储", "挂载链路"):
        d = next(x for x in sysj["devices"] if x["id"] == cid)
        if kind == "设备":
            new = param_form(f"f-d-{cid}", cat["devices"][d["type"]]["params"], d.get("params", {}), f"{cid} ({d['type']}) 参数")
            if new is not None:
                d["params"] = new
        elif kind == "设备存储":
            dm = cat["devices"][d["type"]]["default_memory"]
            mem = d.setdefault("memory", {"type": dm["type"]})
            allowed = cat["devices"][d["type"]]["allowed_memory"]
            mtype = st.selectbox("存储类型", allowed, index=allowed.index(mem.get("type", dm["type"])))
            cur = dict(dm["params"]) if mtype == dm["type"] else {}
            cur.update(mem.get("params", {}) if mem.get("type", dm["type"]) == mtype else {})
            new = param_form(f"f-m-{cid}-{mtype}", cat["memories"][mtype], cur, f"{cid} 本地存储（{mtype}）参数")
            if new is not None:
                mem["type"] = mtype
                mem["params"] = new
        else:
            lk = d.setdefault("attach", {"to": "cpu0", "link": {"type": "PCIe"}}).setdefault("link", {"type": "PCIe"})
            types = list(cat["links"].keys())
            lt = st.selectbox("链路类型", types, index=types.index(lk.get("type", "PCIe")) if lk.get("type", "PCIe") in types else 0)
            parents = ["cpu0"] + [s["id"] for s in sysj.get("switches", [])]
            cur_p = d["attach"].get("to", "cpu0")
            d["attach"]["to"] = st.selectbox("挂载到", parents, index=parents.index(cur_p) if cur_p in parents else 0)
            new = param_form(f"f-l-{cid}-{lt}", cat["links"][lt], lk.get("params", {}) if lk.get("type") == lt else {},
                             f"{cid} 挂载链路（{lt}）参数")
            if new is not None:
                lk["type"] = lt
                lk["params"] = new
    elif kind in ("交换机", "交换机链路"):
        s = next(x for x in sysj["switches"] if x["id"] == cid)
        if kind == "交换机":
            new = param_form(f"f-s-{cid}", cat["switches"][s["type"]], s.get("params", {}), f"交换机 {cid} 参数")
            if new is not None:
                s["params"] = new
        else:
            lk = s.setdefault("attach", {"to": "cpu0", "link": {"type": s["type"]}}).setdefault("link", {"type": s["type"]})
            lt = lk.get("type", s["type"])
            new = param_form(f"f-sl-{cid}", cat["links"][lt], lk.get("params", {}), f"交换机 {cid} 上行链路（{lt}）参数")
            if new is not None:
                lk["params"] = new
    if new is not None:
        rerun()
    st.caption("修改后记得在上方“保存为我的系统配置”。数值超出上下界无法输入；参数间关系约束（如 HBM3E 速率上限、"
               "CXL 版本特性、预算）由 C++ 校验器检查。")


def add_remove_budget():
    h("从模板添加设备 / 交换机")
    tmpls = core.list_configs("devices")
    c1, c2, c3 = st.columns([3, 1, 1])
    with c1:
        tname = labeled_select("模板", [(n, f"{n} — {core.load_json(p).get('description', '')}") for n, p in tmpls.items()])
    t = core.load_json(tmpls[tname])
    default_id = t.get("id") or (t["type"].lower().split("_")[-1] + str(len(ss.system.get("devices", []))))
    nid = c2.text_input("新设备 id", value=default_id)
    c3.markdown("&nbsp;")
    if c3.button("添加"):
        obj = {k: copy.deepcopy(v) for k, v in t.items() if k not in ("template", "description", "kind")}
        obj["id"] = nid
        if t.get("kind") == "switch":
            ss.system.setdefault("switches", []).append(obj)
        else:
            ss.system.setdefault("devices", []).append(obj)
        rerun()
    h("删除设备 / 交换机")
    ids = [d["id"] for d in ss.system.get("devices", [])] + [s["id"] for s in ss.system.get("switches", [])]
    c1, c2 = st.columns([3, 1])
    rid = c1.selectbox("选择要删除的组件", ids) if ids else None
    c2.markdown("&nbsp;")
    if rid and c2.button("删除"):
        ss.system["devices"] = [d for d in ss.system.get("devices", []) if d["id"] != rid]
        ss.system["switches"] = [s for s in ss.system.get("switches", []) if s["id"] != rid]
        rerun()
    h("更换主机 CPU 模板")
    hosts = core.list_configs("hosts")
    c1, c2 = st.columns([3, 1])
    with c1:
        hn = labeled_select("主机模板", [(n, f"{n} — {core.load_json(p).get('description', '')}") for n, p in hosts.items()])
    c2.markdown("&nbsp;")
    if c2.button("替换主机"):
        hj = core.load_json(hosts[hn])
        ss.system["host"] = {k: v for k, v in hj.items() if k not in ("template", "description")}
        rerun()
    h("预算约束（协同设计赛道）")
    b = ss.system.setdefault("budget", {"enabled": False, "power_w": 2500, "cost_usd": 60000})
    with st.form("budget"):
        en = st.checkbox("启用预算约束", value=bool(b.get("enabled")))
        pw = st.number_input("功耗上限 (W)", min_value=0.0, value=float(b.get("power_w") or 2500))
        cu = st.number_input("成本上限 ($)", min_value=0.0, value=float(b.get("cost_usd") or 60000))
        if st.form_submit_button("应用"):
            ss.system["budget"] = {"enabled": en, "power_w": pw, "cost_usd": cu}
            rerun()
    desc = st.text_area("系统说明（保存在配置的 description 字段）", value=ss.system.get("description", ""), height=80)
    if desc != ss.system.get("description", ""):
        ss.system["description"] = desc


# ---------------------------------------------------------------------------
# ③ 任务与策略
# ---------------------------------------------------------------------------
def choose_workload(key="wl"):
    opts = [f"{b['id']} · {b['title']}" for b in tasks.BASELINES] + ["R-MAT 合成图（可扩展性）", "自定义 workload JSON"]
    choice = st.selectbox("任务", opts, key=f"{key}-task")
    if choice.startswith("R-MAT"):
        sc = st.slider("R-MAT scale（节点数 = 2^scale，边数 = 16×节点）", 14, 27, 20, key=f"{key}-scale")
        return f"S{sc}", tasks.rmat_workload(sc)
    if choice.startswith("自定义"):
        txt = st.text_area("workload JSON", value=json.dumps(tasks.BASELINES[1]["workload"], ensure_ascii=False, indent=2),
                           height=300, key=f"{key}-json")
        try:
            return "custom", json.loads(txt)
        except json.JSONDecodeError as ex:
            st.error(f"JSON 错误：{ex}")
            return None, None
    b = tasks.BASELINES[opts.index(choice)]
    st.caption(b["description"])
    return b["id"], copy.deepcopy(b["workload"])


def run_and_store(tid, wl, system, system_name, strategy, strategy_name):
    t0 = time.time()
    r = core.simulate(system, wl, strategy)
    ss.history.append({"label": f"{tid} | {system_name} | {strategy_name}", "task": tid, "system": system_name,
                       "strategy": strategy_name, "result": r, "wall": time.time() - t0})
    return len(ss.history) - 1


def summary_metrics(r):
    c = st.columns(5)
    c[0].metric("仿真完成时间", viz.fmt_time(r["makespan_s"]))
    c[1].metric("能耗", f"{r['energy']['total_j']:,.1f} J")
    c[2].metric("平均功率", f"{r['energy']['avg_power_w']:,.0f} W")
    c[3].metric("数据搬运", viz.fmt_bytes(r["transfers"]["total_GB"]))
    c[4].metric("分块 / 任务数", f"{r['graph']['chunks']} / {r['graph']['tasks']}")
    st.caption(f"仿真墙钟 {r['wall_time_s'] * 1000:.1f} ms，事件 {r['sim_events']:,} 个")


def page_run():
    st.title("任务与策略")
    show_flash()
    st.markdown(f"当前系统：**{ss.system_name}**（在“系统配置”页修改）")
    v = validated()
    if not v["ok"]:
        st.error("当前系统配置不合法，请先修正：\n\n" + "\n".join("- " + e for e in v["errors"]))
        return
    left, right = st.columns([1, 1])
    with left:
        h("1. 选择任务")
        tid, wl = choose_workload()
        if wl:
            ch = core.characterize(wl)
            if ch.get("ok"):
                show_df(pd.DataFrame([{"阶段": viz.STAGE_CN.get(k, k), "GFLOP": round(x["GFLOP"], 2),
                                       "访存 GB": round(x["GB_accessed"], 2), "随机访问 (百万次)": round(x["random_accesses_M"], 2),
                                       "算术强度": round(x["arith_intensity"], 2)} for k, x in ch["stages"].items()]))
    with right:
        h("2. 选择 / 编辑策略")
        config_locations("strategies")
        ents = configstore.entries("strategies")
        names = list(ents)
        c1, c2, c3 = st.columns([3, 1, 1])
        cur = ss.strategy_name if ss.strategy_name in names else names[0]
        with c1:
            sel = labeled_select("选择策略", config_items("strategies"), names.index(cur))
        c2.markdown("&nbsp;")
        if c2.button("载入"):
            load_strategy(sel)
            rerun()
        c3.markdown("&nbsp;")
        if c3.button("刷新列表", key="refresh-st"):
            rerun()
        txt = st.text_area("策略 JSON（可直接编辑；含 \"plugin\" 字段时由 C++ 插件规划）", value=ss.strategy_text, height=320)
        ss.strategy_text = txt
        state = "**● 有未保存的修改**" if strategy_dirty() else "已与文件一致"
        st.caption(f"正在编辑：{ss.strategy_name}　{state}")
    try:
        strat = json.loads(ss.strategy_text)
    except json.JSONDecodeError as ex:
        st.error(f"策略 JSON 错误：{ex}")
        return
    h("保存为我的策略")
    saved = save_panel("strategies", strat, ss.strategy_name, "save-strategy")
    if saved:
        strat["name"] = saved
        ss.strategy_text = json.dumps(strat, ensure_ascii=False, indent=2)
        ss.strategy_name = saved
        ss.strategy_snapshot = ss.strategy_text
        rerun()
    if configstore.entries("strategies").get(ss.strategy_name, {}).get("source") == "user":
        if st.button(f"🗑 删除我的策略 {ss.strategy_name}"):
            p = configstore.delete("strategies", ss.strategy_name)
            ss.flash = f"已删除 `{configstore.rel(p)}`（当前编辑内容仍保留，可重新保存）"
            ss.strategy_snapshot = ""
            rerun()
    h("3. 运行")
    c1, c2, _ = st.columns([1, 1, 3])
    plan_clicked = c2.button("仅查看规划结果")
    if c1.button("▶ 运行仿真", type="primary") and wl:
        with st.spinner("C++ 模拟器运行中…"):
            i = run_and_store(tid, wl, ss.system, ss.system_name, strat, strat.get("name", ss.strategy_name))
        r = ss.history[i]["result"]
        if r["status"] != "ok":
            st.error("仿真失败：\n\n" + "\n".join("- " + e for e in r.get("errors", [])))
        else:
            st.success(f"完成：{ss.history[i]['label']}（详细分析见“结果分析”页，多配置比较见“多配置对比”页）")
            summary_metrics(r)
            h("各阶段起止时间")
            show_fig(viz.stage_bars(r))
            for w in r.get("warnings", [])[:8]:
                st.warning(w)
    if plan_clicked and wl:
        p = core.plan(ss.system, wl, strat)
        if p.get("ok"):
            st.json(p["strategy"])
        else:
            st.error("\n".join(p.get("errors", [])))


# ---------------------------------------------------------------------------
# ④ 结果分析
# ---------------------------------------------------------------------------
def run_label(i):
    return f"#{i + 1} {ss.history[i]['label']}"


def page_analysis():
    st.title("结果分析")
    if not ss.history:
        st.info("还没有运行记录。请先在“任务与策略”页运行仿真。")
        return
    i = labeled_select("选择运行记录", [(k, run_label(k)) for k in range(len(ss.history))], len(ss.history) - 1)
    r = ss.history[i]["result"]
    if r["status"] != "ok":
        st.error("该次仿真失败：\n\n" + "\n".join("- " + e for e in r.get("errors", [])))
        if r.get("strategy"):
            st.json(r["strategy"])
        return
    summary_metrics(r)
    tabs = st.tabs(["阶段与时间线", "设备与链路", "存储与数据搬运", "Roofline", "策略与告警", "原始结果"])
    with tabs[0]:
        h("各阶段起止时间（阶段之间可以重叠）")
        show_fig(viz.stage_bars(r))
        h("设备执行时间线（可拖选放大）")
        show_fig(viz.gantt(r))
        if r["timeline"].get("truncated"):
            st.caption("时间线记录已截断（只显示前 N 条）。")
        rows = []
        for s, v in r["stages"].items():
            for d, t in v["device_time_s"].items():
                rows.append({"阶段": viz.STAGE_CN.get(s, s), "设备": d, "设备忙碌 (s)": round(t, 6)})
        show_df(pd.DataFrame(rows))
    with tabs[1]:
        devs = [d for d in r["devices"] if d["has_compute"]]
        h("计算设备利用率（忙碌时间 / 总时间）")
        show_fig(viz.hbar([d["id"] for d in devs], [d["utilization"] for d in devs], "利用率", viz.SERIES[0], xmax=1.0))
        lk = sorted([l for l in r["links"] if l["kind"] != "MEDIA" and (l["up_GB"] + l["down_GB"]) > 0],
                    key=lambda l: -l["utilization"])
        if lk:
            h("互连链路平均利用率（取较繁忙方向）")
            show_fig(viz.hbar([f"{l['id']} ({l['kind']})" for l in lk], [l["utilization"] for l in lk], "利用率",
                              viz.SERIES[6], xmax=1.0))
            show_df(pd.DataFrame([{"链路": l["id"], "类型": l["kind"], "有效带宽 GB/s": round(l["bw_GBps"], 1),
                                   "上行 GB": round(l["up_GB"], 3), "下行 GB": round(l["down_GB"], 3),
                                   "利用率": f"{l['utilization']:.1%}"} for l in lk]))
        show_df(pd.DataFrame([{"设备": d["id"], "类型": d["type"], "忙碌 s": round(d["busy_s"], 6), "kernel 数": d["kernels"],
                               "动态能耗 J": round(d["dynamic_energy_j"], 2), "静态能耗 J": round(d["idle_energy_j"], 2),
                               **({"存算一体耐久消耗": f"{d['endurance_used']:.2e}"} if "endurance_used" in d else {})}
                              for d in r["devices"]]))
    with tabs[2]:
        h("存储层级：峰值占用与容量")
        show_fig(viz.memory_bars(r))
        bk = r["transfers"]["by_kind"]
        if bk:
            ks = sorted(bk, key=lambda k: -bk[k]["GB"])
            h("数据搬运量（按类型）")
            show_fig(viz.hbar([viz.TRANSFER_CN.get(k, k) for k in ks], [bk[k]["GB"] for k in ks], "GB",
                              viz.SERIES[1], fmt="{:.2f} GB"))
        show_df(pd.DataFrame([{"存储": m["id"], "类型": m["kind"], "容量 GB": round(m["capacity_GB"], 1),
                               "峰值占用 GB": round(m["peak_used_GB"], 3), "读 GB": round(m["read_GB"], 3),
                               "写 GB": round(m["written_GB"], 3)} for m in r["memories"]]))
        e = r["energy"]
        st.markdown(f"**能耗分解**：计算 {e['compute_j']:,.1f} J · 传输 {e['transfer_j']:,.1f} J · 静态 {e['static_j']:,.1f} J")
    with tabs[3]:
        devs = sorted({p["device"] for p in r["roofline"] if p["GFLOP"] > 0})
        if devs:
            dsel = st.selectbox("选择设备（每次显示一个设备的屋顶线与其上的 kernel；点上的编号对应下表）", devs)
            show_fig(viz.roofline(r, dsel))
            pts = viz.roofline_points(r, dsel)
            show_df(pd.DataFrame([{"编号": k + 1, "阶段": viz.STAGE_CN.get(p["stage"], p["stage"]), "kernel": p["kernel"],
                                   "算子": p["op"], "精度": p["precision"], "次数": p["count"], "GFLOP": round(p["GFLOP"], 3),
                                   "访存 GB": round(p["GB"], 3), "耗时 s": round(p["time_s"], 6),
                                   "算术强度": round(p["arith_intensity"], 3), "实测 GFLOPS": round(p["achieved_GFLOPS"], 1),
                                   "该精度峰值 GFLOPS": round(p["peak_GFLOPS"], 1)} for k, p in enumerate(pts)]))
    with tabs[4]:
        for w in r.get("warnings", []):
            st.warning(w)
        h("任务图摘要")
        st.json(r["graph"])
        h("最终生效的策略（插件规划 / 自动分块之后）")
        st.json(r["strategy"])
    with tabs[5]:
        st.download_button("下载结果 JSON", json.dumps(r, ensure_ascii=False), file_name="hgcsim_result.json",
                           mime="application/json")
        st.json({k: v for k, v in r.items() if k != "timeline"})


# ---------------------------------------------------------------------------
# ⑤ 多配置对比（最多 6 个）
# ---------------------------------------------------------------------------
CURRENT_SYS = "__current__"


def page_compare():
    st.title("多配置对比")
    st.markdown(f"最多同时对比 **{MAX_COMPARE}** 个运行结果，依次标记为 A~F。可以在多个系统配置上一次性运行同一任务，"
                "也可以从已有运行记录中挑选（例如同一系统上的不同策略）。")
    mode = st.radio("对比来源", ["在多个系统配置上运行同一任务", "从运行记录中选择"], horizontal=True)
    if mode.startswith("在多个"):
        ents = configstore.entries("systems")
        items = [(CURRENT_SYS, f"当前编辑中的系统（{ss.system_name}）")] + config_items("systems")
        default = [n for n in ["default_pc", "hetero_lab"] if n in ents]
        picked = labeled_multiselect(f"系统配置（最多 {MAX_COMPARE} 个）", items, default, MAX_COMPARE)
        c1, c2 = st.columns([1, 1])
        with c1:
            tid, wl = choose_workload(key="cmp")
        with c2:
            sname = labeled_select("策略（所有系统使用同一策略）",
                                   [("__current_st__", f"当前编辑中的策略（{ss.strategy_name}）")] + config_items("strategies"))
            st.caption("提示：不同系统的设备 id 可能不同；固定写了设备 id 的 JSON 策略只适用于含这些设备的系统，"
                       "C++ 插件策略会按每个系统自动规划。")
        if st.button("▶ 运行对比", type="primary", disabled=not picked or not wl):
            if sname == "__current_st__":
                try:
                    strat, stn = json.loads(ss.strategy_text), ss.strategy_name
                except json.JSONDecodeError as ex:
                    st.error(f"当前策略 JSON 错误：{ex}")
                    return
            else:
                strat, stn = configstore.load("strategies", sname), sname
            ids = []
            bar = st.progress(0.0)
            for k, n in enumerate(picked):
                sysj, sysn = (ss.system, ss.system_name) if n == CURRENT_SYS else (configstore.load("systems", n), n)
                ids.append(run_and_store(tid, wl, sysj, sysn, strat, stn))
                bar.progress((k + 1) / len(picked))
            ss.compare_ids = ids
    else:
        if not ss.history:
            st.info("还没有运行记录。")
            return
        dflt = [i for i in ss.compare_ids if i < len(ss.history)] or list(range(max(0, len(ss.history) - 2), len(ss.history)))
        ss.compare_ids = labeled_multiselect(f"运行记录（最多 {MAX_COMPARE} 个）",
                                             [(k, run_label(k)) for k in range(len(ss.history))], dflt, MAX_COMPARE)
    ids = [i for i in ss.compare_ids if i < len(ss.history)][:MAX_COMPARE]
    if not ids:
        return
    show_comparison(ids)


def show_comparison(ids):
    runs = [ss.history[i] for i in ids]
    labels = [f"{viz.RUN_LETTERS[k]} · {h_['system']}/{h_['strategy']}" for k, h_ in enumerate(runs)]
    st.markdown("---")
    rows = []
    ok_idx = []
    base = None
    for k, (lab, h_) in enumerate(zip(labels, runs)):
        r = h_["result"]
        if r["status"] == "ok":
            ok_idx.append(k)
            base = base if base is not None else r["makespan_s"]
            b = r.get("budget", {})
            rows.append({"运行": lab, "任务": h_["task"], "完成时间 (s)": round(r["makespan_s"], 5),
                         "相对 A 的加速比": round(base / r["makespan_s"], 2) if base else None,
                         "能耗 (J)": round(r["energy"]["total_j"], 1), "平均功率 (W)": round(r["energy"]["avg_power_w"]),
                         "数据搬运 (GB)": round(r["transfers"]["total_GB"], 2), "分块": r["graph"]["chunks"],
                         "系统成本 ($)": round(b.get("cost_usd", 0)), "系统峰值功耗 (W)": round(b.get("tdp_w", 0)),
                         "告警数": len(r.get("warnings", []))})
        else:
            rows.append({"运行": lab, "任务": h_["task"], "完成时间 (s)": None, "相对 A 的加速比": None,
                         "能耗 (J)": None, "平均功率 (W)": None, "数据搬运 (GB)": None, "分块": None,
                         "系统成本 ($)": None, "系统峰值功耗 (W)": None, "告警数": None})
    h("汇总")
    show_df(pd.DataFrame(rows))
    for k, h_ in enumerate(runs):
        if h_["result"]["status"] != "ok":
            st.error(f"{labels[k]} 运行失败：" + "；".join(h_["result"].get("errors", [])))
    if not ok_idx:
        return
    L = [labels[k] for k in ok_idx]
    R = [runs[k]["result"] for k in ok_idx]
    if len({runs[k]["task"] for k in ok_idx}) > 1:
        st.warning("所选运行不是同一个任务，完成时间之间不能直接比较。")
    t1, t2, t3, t4 = st.tabs(["完成时间与阶段", "设备利用率", "数据搬运与能耗", "逐个查看"])
    with t1:
        h("完成时间")
        fig = viz.cmp_makespan(L, [r["makespan_s"] for r in R])
        fig.update_traces(marker_color=[viz.RUN_COLORS[k] for k in ok_idx])
        show_fig(fig)
        h("各阶段跨度")
        fig = viz.cmp_stages(L, R)
        for tr, k in zip(fig.data, ok_idx):
            tr.marker.color = viz.RUN_COLORS[k]
        show_fig(fig)
    with t2:
        h("计算设备利用率（— 表示该系统没有这个设备）")
        show_fig(viz.cmp_util_heatmap(L, R))
    with t3:
        h("数据搬运量（按类型）")
        fig = viz.cmp_transfers(L, R)
        for tr, k in zip(fig.data, ok_idx):
            tr.marker.color = viz.RUN_COLORS[k]
        show_fig(fig)
        h("能耗分解")
        show_fig(viz.cmp_energy(L, R))
    with t4:
        xmax = max(r["makespan_s"] for r in R) * 1.02
        st.caption("各运行使用相同的时间轴范围，便于直接比较。")
        cols = st.columns(2)
        for j, (lab, r) in enumerate(zip(L, R)):
            with cols[j % 2]:
                st.markdown(f"**{lab}** · {viz.fmt_time(r['makespan_s'])}")
                show_fig(viz.stage_bars(r, x_max=xmax))
                for w in r.get("warnings", [])[:3]:
                    st.caption("⚠ " + w)
    df = pd.DataFrame(rows)
    st.download_button("下载对比汇总 CSV", df.to_csv(index=False).encode("utf-8-sig"), file_name="hgcsim_compare.csv",
                       mime="text/csv")


# ---------------------------------------------------------------------------
# ⑥ 基线测试套件
# ---------------------------------------------------------------------------
def page_suite():
    st.title("基线测试套件")
    st.markdown("批量运行全部基线任务（B1–B6）与 R-MAT 可扩展性扫描。"
                f"全套测试墙钟时间上限 **{suite.WALL_LIMIT_S:.0f} 秒**。系统与策略列表包含内置配置和我的配置。")
    sents = configstore.entries("systems")
    stents = configstore.entries("strategies")
    c1, c2 = st.columns(2)
    with c1:
        sysnames = labeled_multiselect("系统", config_items("systems"), [n for n in suite.DEFAULT_SYSTEMS if n in sents])
    with c2:
        stnames = labeled_multiselect("策略（第一个作为加速比基准）", config_items("strategies"),
                                      [n for n in suite.DEFAULT_STRATEGIES if n in stents])
    use_cur = st.checkbox("同时运行“系统配置”页中正在编辑的系统", value=False)
    use_cur_st = st.checkbox("同时运行“任务与策略”页中正在编辑的策略", value=False)
    scaling = st.checkbox("包含 R-MAT 可扩展性扫描", value=True)
    if st.button("▶ 运行套件", type="primary"):
        systems = {n: configstore.load("systems", n) for n in sysnames}
        if use_cur:
            systems["编辑中:" + ss.system_name] = ss.system
        strategies = {n: configstore.load("strategies", n) for n in stnames}
        if use_cur_st:
            try:
                strategies["编辑中:" + ss.strategy_name] = json.loads(ss.strategy_text)
            except json.JSONDecodeError:
                st.error("当前策略 JSON 无效")
                return
        if not systems or not strategies:
            st.warning("请至少选择一个系统和一个策略")
            return
        if len(strategies) > len(viz.SERIES):
            st.warning(f"一次最多比较 {len(viz.SERIES)} 个策略，其余策略不会在图中单独着色。")
        bar = st.progress(0.0)
        msg = st.empty()

        def prog(p, m):
            bar.progress(min(1.0, p))
            msg.caption(m)

        ss.suite_result = suite.run_suite(systems, strategies, include_scaling=scaling, progress=prog)
    res = ss.get("suite_result")
    if not res:
        return
    if res["within_limit"]:
        st.success(f"完成 {res['completed']}/{res['jobs']} 次仿真，墙钟时间 {res['wall_s']:.2f} s（< {suite.WALL_LIMIT_S:.0f} s）")
    else:
        st.error(f"未在 {suite.WALL_LIMIT_S:.0f} s 内完成：{res['completed']}/{res['jobs']}，{res['wall_s']:.1f} s")
    df = pd.DataFrame(res["rows"])
    ok = df[df["status"] == "ok"].copy()
    if not ok.empty:
        ok["配置"] = ok["system"] + " / " + ok["strategy"]
        piv = ok.pivot_table(index="task", columns="配置", values="makespan_s", aggfunc="first", sort=False)
        h("仿真完成时间（秒）")
        show_df(piv.reset_index().round(4))
        for sname in ok["system"].unique():
            rows = [{"task": r["task"], "label": r["strategy"], "makespan": r["makespan_s"]}
                    for _, r in ok[ok["system"] == sname].iterrows()]
            h(f"系统 {sname}：各策略完成时间")
            show_fig(viz.compare_bars(rows))
        sp = [c for c in ok.columns if c.startswith("speedup_vs_")]
        if sp:
            piv2 = ok.pivot_table(index="task", columns="配置", values=sp[0], aggfunc="first", sort=False)
            h(f"加速比（相对 {sp[0][len('speedup_vs_'):]}）")
            show_df(piv2.reset_index().round(2))
    bad = df[df["status"] != "ok"]
    if not bad.empty:
        h("失败的仿真")
        show_df(bad[["task", "system", "strategy", "error"]])
    c1, c2 = st.columns(2)
    c1.download_button("下载结果 CSV", df.to_csv(index=False).encode("utf-8-sig"), file_name="hgcsim_suite.csv", mime="text/csv")
    c2.download_button("下载结果 JSON", json.dumps(res, ensure_ascii=False), file_name="hgcsim_suite.json",
                       mime="application/json")


# ---------------------------------------------------------------------------
# ⑦ 参数参考
# ---------------------------------------------------------------------------
def page_params():
    st.title("参数参考（默认值 / 上下界）")
    st.markdown("所有参数在 C++ 中定义并校验；上界取自截至 2026 年的公开器件参数，出处见 `docs/PARAM_SOURCES.md`。"
                "性能指标（峰值算力、带宽）均由这些结构参数推导，不能直接填写。")
    cat = catalog()
    groups = {"计算设备": cat["devices"], "存储层级": cat["memories"], "互连链路": cat["links"], "交换机": cat["switches"]}
    g = st.radio("类别", list(groups), horizontal=True)
    items = groups[g]
    name = st.selectbox("组件类型", list(items))
    specs = items[name]["params"] if g == "计算设备" else items[name]
    show_df(pd.DataFrame([{"参数": s["name"], "默认值": s["default"], "下界": s["min"], "上界": s["max"], "单位": s["unit"],
                           "取值": ", ".join(str(c) for c in s.get("choices", [])), "说明": s["desc"]} for s in specs]))
    if g == "计算设备":
        dm = items[name]["default_memory"]
        st.caption(f"默认本地存储：{dm['type']} {json.dumps(dm['params'], ensure_ascii=False)}；允许：{items[name]['allowed_memory']}")


HANDLERS = dict(zip(PAGES, [page_overview, page_system, page_run, page_analysis, page_compare, page_suite, page_params]))
HANDLERS[page]()
