// =============================================================================
// workload.hpp : 大规模图聚类负载模型 + 调度策略
//
// 流水线（解耦式深度图聚类）：
//   [load]               数据集（特征 X、原始边表 G）从存储读入 / 暂存
//   [feature_extraction] MLP/自编码器编码 X -> Z                  —— 计算密集 (GEMM)
//   [graph_construction] 原始边表 -> 归一化 CSR (排序/度统计)      —— 内存密集 (SORT/SCATTER)
//                        或无图时 kNN 建图 (距离 GEMM + Top-k)
//   [clustering]         T 轮迭代：图传播 SpMM（不规则 gather）
//                        + 簇分配 GEMM + 中心更新归约（全局同步）—— 不规则访存 + 迭代
// 所有工作量（FLOPs、字节数、随机访问次数、工作集）均由数据集参数解析推导，
// 不做真实数值计算。数据按 P 个分块（chunk）切分，跨分块的邻居访问通过
// “割边比例”估计光环 (halo) 交换量。
// =============================================================================
#pragma once
#include <map>
#include <string>
#include <vector>

#include "hgcsim/device.hpp"
#include "hgcsim/params.hpp"

namespace hgc {

class System;

struct Dataset {
  std::string name;
  double nodes = 0;
  double edges = 0;          // 邻接矩阵非零元（有向计数）
  double feature_dim = 0;
  int clusters = 0;
  bool has_graph = true;     // false 时需 kNN 建图
  double knn_k = 10;
  double degree_skew = 0.5;  // 度分布偏斜 0~0.95
  double locality = 0.3;     // 节点编号局部性 0~1（range 划分的割边减少比例）
  double metis_cut_base = 0.1;
  double feature_bytes = 4;
  double index_bytes = 4;
};

struct Algorithm {
  std::string name;
  std::vector<double> encoder_dims{256, 64};
  int encoder_epochs = 0;
  double train_flops_factor = 6;  // 训练（前向+反向，含解码器）相对推理的倍数
  std::string method = "kmeans";  // kmeans | spectral
  int iterations = 20;
  int prop_hops = 2;
  bool prop_every_iter = false;
  int lanczos_block = 32;
  int kmeans_after = 10;
  double knn_candidate_ratio = 1.0;
};

struct Workload {
  std::string name;
  Dataset ds;
  Algorithm alg;
  static bool from_json(const json& j, Workload& w, std::vector<std::string>& errors);
  json to_json() const;
};

// ---------------- 调度策略 ----------------
struct StageAssign {
  std::vector<std::pair<std::string, double>> devices;  // 设备 id + 份额
  Precision prec = Precision::FP32;
  bool zero_copy = false;
  bool dynamic = false;  // 动态分配：分块就绪时再选择设备
};

struct Strategy {
  std::string name = "unnamed";
  std::string dataset_location;            // 数据集初始所在设备（SSD/CSD/内存设备）
  std::map<std::string, std::string> placement;  // features / graph / spill -> 存储 id
  int num_chunks = 0;                      // 0 = auto
  std::string partition = "range";         // range | random | metis
  bool overlap = false;
  int prefetch_depth = 1;
  bool halo_cache = false;                 // halo 成员整块缓存到本地（而非逐行临时拉取）
  // 键: feature_extraction / graph_construction / clustering
  //     以及可选的 clustering.propagation / clustering.assignment（覆盖 clustering）
  std::map<std::string, StageAssign> stages;
  std::string leader;                      // 归约/同步所在设备
  std::string plugin;                      // C++ 调度插件 .so 路径
  json plugin_args = json::object();
  json raw;

  static bool from_json(const json& j, Strategy& s, std::vector<std::string>& errors);
  json to_json() const;
};

extern const char* const kStages[3];  // feature_extraction / graph_construction / clustering

// ---------------- 任务图 ----------------
struct Chunk {
  int id = -1;
  std::string name;
  std::string tensor;
  double bytes = 0;
  std::string home;   // 初始/暂存/溢出 所在存储
  int producer = -1;  // 产生它的任务 (-1 表示数据集原始数据)
  bool final_output = false;
};

struct TaskInput {
  enum Mode { FULL, HALO } mode = FULL;
  int chunk = -1;                 // FULL 模式
  std::vector<int> group;         // HALO 模式：其它分块
  double bytes_per_member = 0;    // HALO：从每个分块 gather 的字节
  double elem = 0;                // HALO：每行字节；<0 表示 STREAM（kernel 执行期间流式读入）
  double accesses_per_member = 0; // HALO：对每个远端分块的随机访问次数（零拷贝时使用）
};

struct Task {
  int id = -1;
  std::string name;
  std::string stage;     // load / partition / feature_extraction / graph_construction / clustering / output
  std::string kind = "kernel";  // kernel | copy
  Kernel k;
  std::string device;    // 静态分配的设备
  std::vector<std::string> candidates;  // 动态分配候选
  bool dynamic = false;
  bool zero_copy = false;  // 允许零拷贝远程访存
  std::vector<TaskInput> inputs;
  std::vector<int> outputs;
  std::vector<int> deps;
  std::string copy_dst;  // copy 任务目标存储
  int chunk = -1, iter = -1;
};

struct TaskGraph {
  std::vector<Chunk> chunks;
  std::vector<Task> tasks;
  int P = 1;
  double cut_ratio = 0;
  json summary;
};

// 依据 workload + strategy + system 生成任务图
bool build_task_graph(const Workload& w, const Strategy& s, const System& sys, TaskGraph& g,
                      std::vector<std::string>& errors);

// 自动选择分块数
int auto_chunks(const Workload& w, const Strategy& s, const System& sys);

// 整个阶段（不分块）的代表性 kernel 序列，用于规划器估计
std::vector<Kernel> stage_kernels(const Workload& w, const std::string& stage, Precision prec);

json workload_characterize(const Workload& w);
std::string default_dataset_location(const System& sys);

}  // namespace hgc
