"""大规模图聚类基线任务定义（前端控制，向 C++ 模拟器下发）。

数据集规模取自公开数据集（Planetoid / OGB / Reddit / SIFT1M）的统计量；
算法配置对应典型的解耦式深度图聚类流程：
  特征提取（自编码器 / MLP 编码） -> 图构建（CSR 归一化 或 kNN 建图） -> 聚类迭代（图传播 + K-means / 谱聚类）
"""
from __future__ import annotations

import copy
from typing import Any, Dict, List

# ---------------------------------------------------------------------------
# 数据集（nodes / edges 为有向非零元计数）
# degree_skew   : 度分布偏斜（越大热点越集中，缓存命中越高）
# locality      : 原始编号局部性（range 划分时割边减少的比例）
# metis_cut_base: METIS 类划分的割边基数（cut ≈ base × log2(P)）
# ---------------------------------------------------------------------------
DATASETS: Dict[str, Dict[str, Any]] = {
    "cora": dict(name="Cora", nodes=2708, edges=10556, feature_dim=1433, clusters=7, has_graph=True,
                 degree_skew=0.5, locality=0.5, metis_cut_base=0.05),
    "ogbn-arxiv": dict(name="ogbn-arxiv", nodes=169343, edges=2332486, feature_dim=128, clusters=40,
                       has_graph=True, degree_skew=0.6, locality=0.3, metis_cut_base=0.07),
    "reddit": dict(name="Reddit", nodes=232965, edges=114615892, feature_dim=602, clusters=41, has_graph=True,
                   degree_skew=0.6, locality=0.2, metis_cut_base=0.12),
    "ogbn-products": dict(name="ogbn-products", nodes=2449029, edges=123718280, feature_dim=100, clusters=47,
                          has_graph=True, degree_skew=0.6, locality=0.35, metis_cut_base=0.04),
    "ogbn-papers100M": dict(name="ogbn-papers100M", nodes=111059956, edges=1615685872, feature_dim=128,
                            clusters=172, has_graph=True, degree_skew=0.7, locality=0.4, metis_cut_base=0.05),
    "sift1m": dict(name="SIFT1M", nodes=1000000, edges=0, feature_dim=128, clusters=100, has_graph=False,
                   knn_k=10, degree_skew=0.2, locality=0.1, metis_cut_base=0.1),
}


def _wl(tid: str, title: str, ds: str, alg: Dict[str, Any], desc: str) -> Dict[str, Any]:
    return {"id": tid, "title": title, "description": desc,
            "workload": {"name": f"{tid}-{DATASETS[ds]['name']}", "dataset": copy.deepcopy(DATASETS[ds]),
                         "algorithm": alg}}


BASELINES: List[Dict[str, Any]] = [
    _wl("B1", "Cora · GAE + K-means（冒烟测试）", "cora",
        dict(name="GAE+KMeans", encoder_dims=[256, 16], encoder_epochs=200, method="kmeans", iterations=50,
             prop_hops=2, prop_every_iter=False),
        "小规模冒烟测试：特征维度高、图很小，异构卸载的启动与传输开销可能得不偿失。"),
    _wl("B2", "ogbn-arxiv · DEC 式深度聚类", "ogbn-arxiv",
        dict(name="DEC/SDCN-like", encoder_dims=[256, 64], encoder_epochs=20, method="kmeans", iterations=50,
             prop_hops=2, prop_every_iter=True),
        "中等规模：每轮聚类迭代都做 2 跳图传播（SpMM），考察计算/访存重叠与迭代同步开销。"),
    _wl("B3", "Reddit · 稠密图谱聚类", "reddit",
        dict(name="Spectral (block Lanczos)", encoder_dims=[128], encoder_epochs=0, method="spectral",
             iterations=30, prop_hops=1, lanczos_block=64, kmeans_after=20),
        "平均度≈492 的稠密图：块 Lanczos 迭代中 SpMM 占主导，考察不规则访存与近内存计算。"),
    _wl("B4", "ogbn-products · S3GC 式可扩展聚类", "ogbn-products",
        dict(name="S3GC-like", encoder_dims=[256, 128], encoder_epochs=10, method="kmeans", iterations=30,
             prop_hops=2, prop_every_iter=False),
        "百万级节点：编码器训练 + 图传播 + K-means，考察多设备负载均衡与图划分。"),
    _wl("B5", "ogbn-papers100M · 外存级图聚类", "ogbn-papers100M",
        dict(name="Dink-Net/S3GC-like", encoder_dims=[256, 64], encoder_epochs=1, method="kmeans", iterations=20,
             prop_hops=2, prop_every_iter=False),
        "1.1 亿节点 / 16 亿边：原始数据≈70GB，超出普通 PC 内存与显存，考察存储层次、CXL 与分块调度。"),
    _wl("B6", "SIFT1M · kNN 建图 + 谱聚类", "sift1m",
        dict(name="kNN graph + Spectral", encoder_dims=[64], encoder_epochs=0, method="spectral", iterations=20,
             prop_hops=1, lanczos_block=32, kmeans_after=20, knn_candidate_ratio=1.0),
        "无图数据：图构建阶段为 O(N²) 距离计算 + Top-k，考察存算一体/矩阵加速器对图构建的加速。"),
]


def rmat_workload(scale: int) -> Dict[str, Any]:
    n = 2 ** scale
    return {"name": f"R-MAT-{scale}",
            "dataset": dict(name=f"R-MAT scale {scale}", nodes=n, edges=16 * n, feature_dim=128, clusters=64,
                            has_graph=True, degree_skew=0.75, locality=0.1, metis_cut_base=0.15),
            "algorithm": dict(name="propagate+KMeans", encoder_dims=[128, 64], encoder_epochs=0, method="kmeans",
                              iterations=20, prop_hops=2, prop_every_iter=False)}


SCALING_SCALES = [18, 20, 22, 24]


def baseline(tid: str) -> Dict[str, Any]:
    for b in BASELINES:
        if b["id"] == tid:
            return copy.deepcopy(b)
    raise KeyError(tid)
