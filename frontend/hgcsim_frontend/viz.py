"""前端可视化（Plotly）。

可读性约定：
  * 图内不放标题（标题由页面的小标题给出），图例放在绘图区上方，二者不会重叠
  * 正文字号 ≥ 12px；文字一律使用墨色，不使用系列颜色
  * 分类颜色按身份固定分配：阶段 → 固定颜色；对比视图中的运行 A~F → 固定颜色
  * 前端固定为浅色主题（.streamlit/config.toml），配色均在浅色背景上校验过
"""
from __future__ import annotations

import math
from typing import Any, Dict, List, Optional, Sequence

import plotly.graph_objects as go

# 固定顺序的分类色（不循环使用）
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
# 对比视图中运行 A~F 的颜色：取前 6 个槽位
RUN_COLORS = SERIES[:6]
RUN_LETTERS = "ABCDEF"
STAGE_ORDER = ["load", "partition", "feature_extraction", "graph_construction", "clustering", "output"]
STAGE_COLOR = {
    "feature_extraction": SERIES[0],
    "graph_construction": SERIES[1],
    "clustering": SERIES[2],
    "partition": SERIES[3],
    "load": SERIES[6],
    "output": SERIES[4],
    "transfer": "#a9a8a2",
}
STAGE_CN = {
    "load": "数据加载/暂存",
    "partition": "图划分",
    "feature_extraction": "特征提取",
    "graph_construction": "图构建",
    "clustering": "聚类迭代",
    "output": "结果汇集",
    "transfer": "数据传输",
}
LINK_COLOR = {"PCIe": SERIES[0], "CXL": SERIES[6], "DDR": SERIES[3], "NVLink": SERIES[5], "MEDIA": "#b9b8b2"}
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#8a8984"
GRID = "#e6e5e0"
SURFACE = "#ffffff"
FONT = '"Source Sans Pro", "Noto Sans CJK SC", "Microsoft YaHei", sans-serif'


def _layout(fig: go.Figure, height: int = 380, legend: bool = True, bottom: int = 10) -> go.Figure:
    fig.update_layout(
        template="plotly_white",
        height=height,
        margin=dict(l=10, r=24, t=44 if legend else 12, b=bottom),
        font=dict(color=INK2, size=13, family=FONT),
        paper_bgcolor=SURFACE,
        plot_bgcolor=SURFACE,
        showlegend=legend,
        legend=dict(orientation="h", yanchor="bottom", y=1.01, xanchor="left", x=0, font=dict(size=13, color=INK2),
                    bgcolor="rgba(0,0,0,0)"),
        hoverlabel=dict(bgcolor="white", font=dict(size=13, color=INK)),
    )
    fig.update_xaxes(gridcolor=GRID, zeroline=False, tickfont=dict(size=12, color=INK2),
                     title_font=dict(size=13, color=INK2), linecolor=GRID)
    fig.update_yaxes(gridcolor=GRID, zeroline=False, tickfont=dict(size=12, color=INK2),
                     title_font=dict(size=13, color=INK2), linecolor=GRID)
    return fig


def fmt_time(s: float) -> str:
    if s is None:
        return "-"
    if s < 1e-3:
        return f"{s * 1e6:.1f} µs"
    if s < 1:
        return f"{s * 1e3:.2f} ms"
    if s < 120:
        return f"{s:.3f} s"
    return f"{s / 60:.2f} min"


def fmt_bytes(gb: float) -> str:
    def num(v: float) -> str:
        return f"{v:.0f}" if abs(v - round(v)) < 1e-6 or v >= 100 else f"{v:.2f}"
    if gb >= 1024:
        return f"{num(gb / 1024)} TB"
    if gb >= 1:
        return f"{num(gb)} GB"
    return f"{gb * 1024:.1f} MB"


# ---------------------------------------------------------------------------
# 系统拓扑：横向树（主机在左，逐级向右），每个叶子独占一行，标签放在节点右侧
# ---------------------------------------------------------------------------
def topology(desc: Dict[str, Any]) -> go.Figure:
    devs = {d["id"]: d for d in desc["devices"]}
    switches = {s["id"]: s for s in desc.get("switches", [])}
    mems = {m["id"]: m for m in desc["memories"]}
    parent: Dict[str, str] = {}
    plink: Dict[str, Dict[str, Any]] = {}
    peers = []
    for l in desc["links"]:
        if l["kind"] == "MEDIA":
            continue
        if l["kind"] == "NVLink":
            peers.append(l)
            continue
        parent[l["a"]] = l["b"]
        plink[l["a"]] = l
    root = desc["devices"][0]["id"]
    children: Dict[str, List[str]] = {}
    for c, p in parent.items():
        children.setdefault(p, []).append(c)

    def sort_key(n: str):
        return (0 if n in switches else 1, devs.get(n, {}).get("type", ""), n)

    pos: Dict[str, tuple] = {}
    row = [0]

    def place(n: str, depth: int):
        kids = sorted(children.get(n, []), key=sort_key)
        if not kids:
            pos[n] = (depth, row[0])
            row[0] += 1
            return
        start = row[0]
        for k in kids:
            place(k, depth + 1)
        pos[n] = (depth, (start + row[0] - 1) / 2.0)

    place(root, 0)
    for n in list(devs) + list(switches):
        if n not in pos:
            pos[n] = (1, row[0])
            row[0] += 1
    nrows = max(row[0], 1)
    maxdepth = max(p[0] for p in pos.values())

    def mem_text(d: Dict[str, Any]) -> str:
        m = d.get("memory")
        if not m:
            return ""
        return f"{m['kind']} {fmt_bytes(m['capacity_GB'])} · {m['peak_bw_GBps']:.0f} GB/s"

    fig = go.Figure()
    # 连线（按链路类型着色），父 -> 子 用直角折线
    used_kinds = set()
    for c, p in parent.items():
        if c not in pos or p not in pos:
            continue
        (x0, y0), (x1, y1) = pos[p], pos[c]
        l = plink[c]
        kind = l["kind"]
        used_kinds.add(kind)
        xm = x0 + 0.28
        fig.add_scatter(x=[x0, xm, xm, x1], y=[y0, y0, y1, y1], mode="lines",
                        line=dict(width=2.5, color=LINK_COLOR.get(kind, MUTED)), hoverinfo="skip", showlegend=False)
        lab = kind
        if l.get("pcie_gen"):
            lab = f"PCIe {l['pcie_gen']}.0"
        if l.get("cxl_version"):
            lab = f"CXL {l['cxl_version']:.1f}"
        fig.add_annotation(x=x1 - 0.06, y=y1, yshift=11, xanchor="right", text=f"{lab} · {l['effective_GBps']:.0f} GB/s",
                           showarrow=False, font=dict(size=12, color=INK2))
    for l in peers:
        a, b = l["a"], l["b"]
        if a in pos and b in pos:
            (x0, y0), (x1, y1) = pos[a], pos[b]
            used_kinds.add("NVLink")
            fig.add_scatter(x=[x0 + 0.08, x0 + 0.08], y=[y0, y1], mode="lines",
                            line=dict(width=2, color=LINK_COLOR["NVLink"], dash="dot"), hoverinfo="skip", showlegend=False)
    # 节点
    groups = [
        ("主机 CPU", [root], "square", SERIES[0], 18),
        ("交换机", [n for n in switches if n in pos], "diamond", SERIES[6], 18),
        ("计算设备", [n for n in devs if n != root and devs[n].get("has_compute") and n in pos], "square", SERIES[0], 15),
        ("存储设备", [n for n in devs if n != root and not devs[n].get("has_compute") and n in pos], "circle", MUTED, 15),
    ]
    for name, ns, sym, col, size in groups:
        if not ns:
            continue
        xs, ys, texts, hov, tpos = [], [], [], [], []
        for n in ns:
            x, y = pos[n]
            xs.append(x)
            ys.append(y)
            if n in switches:
                s = switches[n]
                texts.append(f"<b>{n}</b><br>{s['type']} 交换机")
                hov.append(f"{n}：{s['type']} 交换机")
                tpos.append("bottom center")
            else:
                d = devs[n]
                mt = mem_text(d)
                hov.append(f"{n} ({d['type']})<br>{mt}<br>功耗 {d['tdp_w']:.0f} W · 成本 ${d['cost_usd']:,.0f}")
                if children.get(n):  # 有下游设备的节点（主机）：标签放在节点下方，避开右侧连线
                    mm = d.get("memory")
                    extra = (f"<br><span style='color:{MUTED}'>{mm['kind']} {fmt_bytes(mm['capacity_GB'])}"
                             f"<br>{mm['peak_bw_GBps']:.0f} GB/s</span>") if mm else ""
                    texts.append(f"<b>{n}</b> {d['type']}" + extra)
                    tpos.append("bottom center")
                else:
                    texts.append(f"<b>{n}</b>  {d['type']}" + (f"  <span style='color:{MUTED}'>{mt}</span>" if mt else ""))
                    tpos.append("middle right")
        fig.add_scatter(x=xs, y=ys, mode="markers+text", name=name, text=texts, textposition=tpos,
                        textfont=dict(size=13, color=INK), hovertext=hov, hoverinfo="text",
                        marker=dict(symbol=sym, size=size, color=col, line=dict(width=2, color="white")))
    for kind in ["PCIe", "CXL", "DDR", "NVLink"]:
        if kind in used_kinds:
            fig.add_scatter(x=[None], y=[None], mode="lines", name=f"{kind} 链路",
                            line=dict(width=3, color=LINK_COLOR[kind], dash="dot" if kind == "NVLink" else "solid"))
    _layout(fig, height=max(340, 58 * nrows + 110))
    fig.update_xaxes(visible=False, range=[-0.75, maxdepth + 2.4])
    fig.update_yaxes(visible=False, range=[max(nrows - 0.4, pos[root][1] + 1.2), -0.9])
    return fig


# ---------------------------------------------------------------------------
# 单次运行
# ---------------------------------------------------------------------------
def gantt(res: Dict[str, Any], max_bars: int = 6000) -> go.Figure:
    """设备时间线：每台设备一行，颜色 = 阶段；传输按目的端汇总为单独一行。"""
    ks = res["timeline"]["kernels"]
    xs = res["timeline"]["transfers"]
    rows: Dict[str, Dict[str, List]] = {}
    for dev, stage, name, t0, t1 in ks[:max_bars]:
        d = rows.setdefault(stage, {"y": [], "base": [], "x": [], "text": []})
        d["y"].append(dev)
        d["base"].append(t0)
        d["x"].append(max(t1 - t0, 1e-12))
        d["text"].append(f"{name}<br>{dev} · {fmt_time(t1 - t0)}")
    fig = go.Figure()
    for st in STAGE_ORDER + [s for s in rows if s not in STAGE_ORDER]:
        if st not in rows:
            continue
        d = rows[st]
        fig.add_bar(y=d["y"], x=d["x"], base=d["base"], orientation="h", name=STAGE_CN.get(st, st),
                    marker=dict(color=STAGE_COLOR.get(st, SERIES[4]), line=dict(width=0)),
                    hovertext=d["text"], hoverinfo="text")
    if xs:
        y, base, x, text = [], [], [], []
        for kind, src, dst, b, t0, t1 in xs[:max_bars]:
            if t1 <= t0:
                continue
            y.append("传输→" + dst)
            base.append(t0)
            x.append(t1 - t0)
            text.append(f"{kind}: {src} → {dst}<br>{b / 2**30:.3f} GB · {fmt_time(t1 - t0)}")
        fig.add_bar(y=y, x=x, base=base, orientation="h", name="数据传输（按目的端）",
                    marker=dict(color=STAGE_COLOR["transfer"], line=dict(width=0)),
                    hovertext=text, hoverinfo="text")
    nrows = len(set(r[0] for r in ks)) + len(set(x[2] for x in xs))
    _layout(fig, height=max(320, 36 * nrows + 110))
    fig.update_layout(barmode="overlay", bargap=0.3)
    fig.update_xaxes(title="仿真时间 (s)")
    fig.update_yaxes(autorange="reversed")
    return fig


def stage_bars(res: Dict[str, Any], x_max: Optional[float] = None, height: Optional[int] = None) -> go.Figure:
    st = res["stages"]
    order = [s for s in STAGE_ORDER if s in st]
    fig = go.Figure()
    for s in order:
        v = st[s]
        fig.add_bar(y=[STAGE_CN.get(s, s)], x=[max(v["span_s"], 1e-12)], base=[v["start_s"]], orientation="h",
                    marker=dict(color=STAGE_COLOR.get(s, SERIES[4])), showlegend=False,
                    hovertext=f"{STAGE_CN.get(s, s)}：{fmt_time(v['start_s'])} → {fmt_time(v['end_s'])}"
                              f"<br>跨度 {fmt_time(v['span_s'])} · 设备累计忙碌 {fmt_time(v['kernel_time_s'])}",
                    hoverinfo="text")
    _layout(fig, height=height or (50 + 42 * len(order)), legend=False)
    fig.update_xaxes(title="仿真时间 (s)", range=[0, x_max] if x_max else None)
    fig.update_yaxes(autorange="reversed")
    return fig


def hbar(labels: Sequence[str], values: Sequence[float], xtitle: str, color: str = SERIES[0],
         fmt: str = "{:.1%}", xmax: Optional[float] = None, colors: Optional[Sequence[str]] = None,
         log: bool = False) -> go.Figure:
    vmax = max([v for v in values if v is not None] or [1])
    fig = go.Figure(go.Bar(y=list(labels), x=list(values), orientation="h",
                           marker=dict(color=list(colors) if colors else color, line=dict(width=0)),
                           text=[fmt.format(v) for v in values], textposition="outside", cliponaxis=False,
                           textfont=dict(size=12, color=INK2), hovertemplate="%{y}: %{x}<extra></extra>"))
    _layout(fig, height=max(200, 34 * len(labels) + 70), legend=False)
    if log:
        fig.update_xaxes(type="log", title=xtitle)
    else:
        fig.update_xaxes(title=xtitle, range=[0, (xmax if xmax else vmax) * 1.22])
    fig.update_yaxes(autorange="reversed")
    return fig


def memory_bars(res: Dict[str, Any]) -> go.Figure:
    mems = [m for m in res["memories"] if m["peak_used_GB"] > 0 or m["read_GB"] > 0 or m["written_GB"] > 0]
    labels = [f"{m['id']} ({m['kind']})" for m in mems]
    fig = go.Figure()
    fig.add_bar(y=labels, x=[m["capacity_GB"] for m in mems], orientation="h", name="容量",
                marker=dict(color="#dcdbd5"), hovertemplate="%{y} 容量 %{x:.1f} GB<extra></extra>")
    fig.add_bar(y=labels, x=[max(m["peak_used_GB"], 1e-6) for m in mems], orientation="h", name="峰值占用",
                marker=dict(color=SERIES[0]), hovertemplate="%{y} 峰值 %{x:.3f} GB<extra></extra>",
                text=[fmt_bytes(m["peak_used_GB"]) for m in mems], textposition="inside", insidetextanchor="end",
                textfont=dict(color="white", size=12))
    _layout(fig, height=max(220, 36 * len(labels) + 90))
    fig.update_layout(barmode="overlay")
    fig.update_xaxes(type="log", title="GB（对数轴）")
    fig.update_yaxes(autorange="reversed")
    return fig


def roofline_points(res: Dict[str, Any], device: str) -> List[Dict[str, Any]]:
    """该设备上有计算量的 kernel，按算术强度排序（编号即在图上的标号）。"""
    pts = [p for p in res["roofline"] if p["device"] == device and p["GFLOP"] > 0 and p["GB"] > 0]
    return sorted(pts, key=lambda p: (p["arith_intensity"], p["kernel"]))


def roofline(res: Dict[str, Any], device: str) -> go.Figure:
    """单个设备的 Roofline：每种出现的精度画一条屋顶线；kernel 点只标编号（名称见下方表格）。"""
    pts = roofline_points(res, device)
    fig = go.Figure()
    if not pts:
        _layout(fig, height=300, legend=False)
        return fig
    bw = pts[0]["mem_bw_GBps"]
    ai_lo = min(min(p["arith_intensity"] for p in pts) / 10, 0.01)
    ai_hi = max(max(p["arith_intensity"] for p in pts) * 10, 100)
    xs = [10 ** (math.log10(ai_lo) + i * (math.log10(ai_hi) - math.log10(ai_lo)) / 200) for i in range(201)]
    roofs: Dict[float, List[str]] = {}
    for p in pts:
        tag = f"{p['precision']} {'矩阵' if p['op'] == 'GEMM' else '向量'}"
        roofs.setdefault(round(p["peak_GFLOPS"], 3), [])
        if tag not in roofs[round(p["peak_GFLOPS"], 3)]:
            roofs[round(p["peak_GFLOPS"], 3)].append(tag)
    for j, (pk, precs) in enumerate(sorted(roofs.items(), reverse=True)):
        if bw <= 0 or pk <= 0:
            continue
        fig.add_scatter(x=xs, y=[min(pk, bw * 1.073741824 * x) for x in xs], mode="lines",
                        name=f"屋顶（{'/'.join(precs)} 峰值 {pk:,.0f}）",
                        line=dict(width=2, color=INK2 if j == 0 else MUTED, dash="solid" if j == 0 else "dash"),
                        hoverinfo="skip")
    by_stage: Dict[str, List[int]] = {}
    for k, p in enumerate(pts):
        by_stage.setdefault(p["stage"], []).append(k)
    for st in [s for s in STAGE_ORDER if s in by_stage] + [s for s in by_stage if s not in STAGE_ORDER]:
        ks = by_stage[st]
        fig.add_scatter(x=[pts[k]["arith_intensity"] for k in ks], y=[pts[k]["achieved_GFLOPS"] for k in ks],
                        mode="markers+text", name=STAGE_CN.get(st, st), text=[str(k + 1) for k in ks],
                        textposition="middle right", textfont=dict(size=13, color=INK),
                        marker=dict(size=13, color=STAGE_COLOR.get(st, SERIES[4]), line=dict(width=2, color="white")),
                        hovertext=[f"{k + 1}. {pts[k]['kernel']} ({pts[k]['precision']})<br>算术强度 "
                                   f"{pts[k]['arith_intensity']:.2f} FLOP/B<br>实测 {pts[k]['achieved_GFLOPS']:.1f} GFLOPS"
                                   f" / 峰值 {pts[k]['peak_GFLOPS']:.0f}<br>调用 {pts[k]['count']} 次，共 "
                                   f"{fmt_time(pts[k]['time_s'])}" for k in ks],
                        hoverinfo="text")
    fig.add_annotation(xref="paper", yref="paper", x=0.01, y=0.99, xanchor="left", yanchor="top", showarrow=False,
                       text=f"{device} 本地存储带宽 {bw:,.0f} GB/s", font=dict(size=12, color=INK2),
                       bgcolor="rgba(255,255,255,0.9)")
    _layout(fig, height=460)
    fig.update_xaxes(type="log", title="算术强度 (FLOP/Byte)")
    fig.update_yaxes(type="log", title="吞吐 (GFLOPS)")
    return fig


# ---------------------------------------------------------------------------
# 多配置对比（最多 6 个运行，颜色固定为 A~F）
# ---------------------------------------------------------------------------
def cmp_makespan(labels: List[str], values: List[float]) -> go.Figure:
    return hbar(labels, values, "仿真完成时间 (s)", fmt="{:.4g} s", colors=RUN_COLORS[:len(labels)])


def cmp_stages(labels: List[str], results: List[Dict[str, Any]]) -> go.Figure:
    stages = [s for s in STAGE_ORDER if any(s in r["stages"] for r in results)]
    fig = go.Figure()
    for i, (lab, r) in enumerate(zip(labels, results)):
        fig.add_bar(y=[STAGE_CN.get(s, s) for s in stages],
                    x=[r["stages"].get(s, {}).get("span_s", 0) for s in stages], orientation="h", name=lab,
                    marker=dict(color=RUN_COLORS[i], line=dict(width=0)),
                    hovertemplate=lab + "<br>%{y}：%{x:.4g} s<extra></extra>")
    _layout(fig, height=max(260, 30 * len(stages) * len(results) + 80))
    fig.update_layout(barmode="group", bargap=0.25, bargroupgap=0.08)
    fig.update_xaxes(title="阶段跨度 (s)")
    fig.update_yaxes(autorange="reversed")
    return fig


def cmp_util_heatmap(labels: List[str], results: List[Dict[str, Any]]) -> go.Figure:
    cols: List[str] = []
    for r in results:
        for d in r["devices"]:
            if d["has_compute"] and d["id"] not in cols:
                cols.append(d["id"])
    z, text = [], []
    for r in results:
        m = {d["id"]: d["utilization"] for d in r["devices"] if d["has_compute"]}
        z.append([m.get(c) for c in cols])
        text.append([("—" if m.get(c) is None else f"{m[c]:.0%}") for c in cols])
    fig = go.Figure(go.Heatmap(z=z, x=cols, y=labels, text=text, texttemplate="%{text}", zmin=0, zmax=1,
                               colorscale=[[0, "#f2f6fc"], [0.5, "#7fb0ea"], [1, "#1f5fae"]],
                               textfont=dict(size=12), hoverongaps=False, xgap=2, ygap=2,
                               colorbar=dict(title="利用率", tickformat=".0%", len=0.9),
                               hovertemplate="%{y}<br>%{x}：%{text}<extra></extra>"))
    _layout(fig, height=max(200, 46 * len(labels) + 90), legend=False)
    fig.update_yaxes(autorange="reversed", gridcolor="rgba(0,0,0,0)")
    fig.update_xaxes(side="top", gridcolor="rgba(0,0,0,0)")
    return fig


TRANSFER_CN = {"fetch": "按需拉取", "halo": "跨分区 halo", "writeback": "逐出写回", "stage_in": "暂存加载",
               "zero_copy": "零拷贝访存", "stream": "流式读入"}


def cmp_transfers(labels: List[str], results: List[Dict[str, Any]]) -> go.Figure:
    kinds = [k for k in TRANSFER_CN if any(k in r["transfers"]["by_kind"] for r in results)]
    fig = go.Figure()
    for i, (lab, r) in enumerate(zip(labels, results)):
        bk = r["transfers"]["by_kind"]
        fig.add_bar(x=[TRANSFER_CN[k] for k in kinds], y=[bk.get(k, {}).get("GB", 0) for k in kinds], name=lab,
                    marker=dict(color=RUN_COLORS[i], line=dict(width=0)),
                    hovertemplate=lab + "<br>%{x}：%{y:.3f} GB<extra></extra>")
    _layout(fig, height=360)
    fig.update_layout(barmode="group", bargap=0.25, bargroupgap=0.06)
    fig.update_yaxes(title="搬运量 (GB)")
    return fig


def cmp_energy(labels: List[str], results: List[Dict[str, Any]]) -> go.Figure:
    parts = [("compute_j", "计算", SERIES[0]), ("transfer_j", "传输", SERIES[1]), ("static_j", "静态", "#b9b8b2")]
    fig = go.Figure()
    for key, name, col in parts:
        fig.add_bar(y=labels, x=[r["energy"][key] for r in results], orientation="h", name=name,
                    marker=dict(color=col, line=dict(width=1, color="white")),
                    hovertemplate="%{y}<br>" + name + "能耗 %{x:,.1f} J<extra></extra>")
    _layout(fig, height=max(220, 40 * len(labels) + 90))
    fig.update_layout(barmode="stack", bargap=0.35, legend_traceorder="normal")
    fig.update_xaxes(title="能耗 (J)")
    fig.update_yaxes(autorange="reversed")
    return fig


def compare_bars(rows: List[Dict[str, Any]]) -> go.Figure:
    """基线套件：rows=[{task, label, makespan}]，对数轴分组柱状图（策略数 ≤ 8）。"""
    labels: List[str] = []
    for r in rows:
        if r["label"] not in labels:
            labels.append(r["label"])
    fig = go.Figure()
    for i, lab in enumerate(labels):
        rs = [r for r in rows if r["label"] == lab]
        fig.add_bar(x=[r["task"] for r in rs], y=[r["makespan"] for r in rs], name=lab,
                    marker=dict(color=SERIES[i] if i < len(SERIES) else MUTED, line=dict(width=0)),
                    hovertemplate=lab + "<br>%{x}: %{y:.4g} s<extra></extra>")
    _layout(fig, height=400)
    fig.update_yaxes(type="log", title="仿真完成时间 (s，对数轴)")
    fig.update_layout(barmode="group", bargap=0.25, bargroupgap=0.05)
    return fig
