#include "hgcsim/workload.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "hgcsim/system.hpp"

namespace hgc {

const char* const kStages[3] = {"feature_extraction", "graph_construction", "clustering"};

// ---------------------------------------------------------------------------
// JSON 解析
// ---------------------------------------------------------------------------
bool Workload::from_json(const json& j, Workload& w, std::vector<std::string>& errors) {
  size_t e0 = errors.size();
  try {
    w.name = j.value("name", "workload");
    json d = j.at("dataset");
    w.ds.name = d.value("name", w.name);
    w.ds.nodes = d.at("nodes").get<double>();
    w.ds.edges = d.value("edges", 0.0);
    w.ds.feature_dim = d.at("feature_dim").get<double>();
    w.ds.clusters = d.at("clusters").get<int>();
    w.ds.has_graph = d.value("has_graph", true);
    w.ds.knn_k = d.value("knn_k", 10.0);
    w.ds.degree_skew = d.value("degree_skew", 0.5);
    w.ds.locality = d.value("locality", 0.3);
    w.ds.metis_cut_base = d.value("metis_cut_base", 0.1);
    w.ds.feature_bytes = d.value("feature_bytes", 4.0);
    w.ds.index_bytes = d.value("index_bytes", 4.0);
    json a = j.value("algorithm", json::object());
    w.alg.name = a.value("name", "graph-clustering");
    if (a.contains("encoder_dims")) w.alg.encoder_dims = a["encoder_dims"].get<std::vector<double>>();
    w.alg.encoder_epochs = a.value("encoder_epochs", 0);
    w.alg.train_flops_factor = a.value("train_flops_factor", 6.0);
    w.alg.method = a.value("method", "kmeans");
    w.alg.iterations = a.value("iterations", 20);
    w.alg.prop_hops = a.value("prop_hops", 2);
    w.alg.prop_every_iter = a.value("prop_every_iter", false);
    w.alg.lanczos_block = a.value("lanczos_block", 32);
    w.alg.kmeans_after = a.value("kmeans_after", 10);
    w.alg.knn_candidate_ratio = a.value("knn_candidate_ratio", 1.0);
  } catch (std::exception& ex) {
    errors.push_back(std::string("workload 解析失败: ") + ex.what());
    return false;
  }
  auto chk = [&](bool c, const std::string& m) {
    if (!c) errors.push_back("workload: " + m);
  };
  chk(w.ds.nodes >= 1 && w.ds.nodes <= 1e10, "nodes 需在 [1, 1e10]");
  chk(w.ds.edges >= 0 && w.ds.edges <= 1e12, "edges 需在 [0, 1e12]");
  chk(w.ds.has_graph ? w.ds.edges > 0 : w.ds.knn_k >= 1, "有图时 edges>0；无图时 knn_k≥1");
  chk(w.ds.feature_dim >= 1 && w.ds.feature_dim <= 1e5, "feature_dim 需在 [1, 1e5]");
  chk(w.ds.clusters >= 2 && w.ds.clusters <= 100000, "clusters 需在 [2, 1e5]");
  chk(w.ds.degree_skew >= 0 && w.ds.degree_skew <= 0.95, "degree_skew 需在 [0, 0.95]");
  chk(w.ds.locality >= 0 && w.ds.locality <= 1, "locality 需在 [0, 1]");
  chk(!w.alg.encoder_dims.empty(), "encoder_dims 不能为空");
  chk(w.alg.method == "kmeans" || w.alg.method == "spectral", "method 只能是 kmeans 或 spectral");
  chk(w.alg.iterations >= 1 && w.alg.iterations <= 1000, "iterations 需在 [1, 1000]");
  chk(w.alg.prop_hops >= 0 && w.alg.prop_hops <= 16, "prop_hops 需在 [0, 16]");
  chk(w.alg.encoder_epochs >= 0 && w.alg.encoder_epochs <= 500, "encoder_epochs 需在 [0, 500]");
  chk(w.alg.knn_candidate_ratio > 0 && w.alg.knn_candidate_ratio <= 1, "knn_candidate_ratio 需在 (0,1]");
  return errors.size() == e0;
}

json Workload::to_json() const {
  return {{"name", name},
          {"dataset",
           {{"name", ds.name},
            {"nodes", ds.nodes},
            {"edges", ds.edges},
            {"feature_dim", ds.feature_dim},
            {"clusters", ds.clusters},
            {"has_graph", ds.has_graph},
            {"knn_k", ds.knn_k},
            {"degree_skew", ds.degree_skew},
            {"locality", ds.locality},
            {"metis_cut_base", ds.metis_cut_base},
            {"feature_bytes", ds.feature_bytes},
            {"index_bytes", ds.index_bytes}}},
          {"algorithm",
           {{"name", alg.name},
            {"encoder_dims", alg.encoder_dims},
            {"encoder_epochs", alg.encoder_epochs},
            {"train_flops_factor", alg.train_flops_factor},
            {"method", alg.method},
            {"iterations", alg.iterations},
            {"prop_hops", alg.prop_hops},
            {"prop_every_iter", alg.prop_every_iter},
            {"lanczos_block", alg.lanczos_block},
            {"kmeans_after", alg.kmeans_after},
            {"knn_candidate_ratio", alg.knn_candidate_ratio}}}};
}

bool Strategy::from_json(const json& j, Strategy& s, std::vector<std::string>& errors) {
  size_t e0 = errors.size();
  s.raw = j;
  s.name = j.value("name", "unnamed");
  s.dataset_location = j.value("dataset_location", "");
  if (j.contains("placement"))
    for (auto it = j["placement"].begin(); it != j["placement"].end(); ++it) s.placement[it.key()] = it.value().get<std::string>();
  json part = j.value("partition", json::object());
  if (part.contains("num_chunks")) {
    if (part["num_chunks"].is_string()) {
      if (part["num_chunks"].get<std::string>() != "auto") errors.push_back("strategy: num_chunks 只能是整数或 \"auto\"");
      s.num_chunks = 0;
    } else {
      s.num_chunks = part["num_chunks"].get<int>();
      if (s.num_chunks < 1 || s.num_chunks > 1024) errors.push_back("strategy: num_chunks 需在 [1, 1024]");
    }
  }
  s.partition = part.value("method", "range");
  if (s.partition != "range" && s.partition != "random" && s.partition != "metis")
    errors.push_back("strategy: partition.method 只能是 range / random / metis");
  json pipe = j.value("pipeline", json::object());
  s.overlap = pipe.value("overlap", false);
  s.prefetch_depth = pipe.value("prefetch_depth", 1);
  s.halo_cache = pipe.value("halo_cache", false);
  if (s.prefetch_depth < 0 || s.prefetch_depth > 16) errors.push_back("strategy: prefetch_depth 需在 [0, 16]");
  s.leader = j.value("leader", "");
  s.plugin = j.value("plugin", "");
  s.plugin_args = j.value("plugin_args", json::object());
  json st = j.value("stages", json::object());
  auto parse_assign = [&](const json& sj, const std::string& name, const StageAssign* parent) {
    StageAssign a;
    if (parent) a = *parent;
    if (sj.contains("devices")) {
      a.devices.clear();
      for (auto& dj : sj["devices"]) {
        if (dj.is_string())
          a.devices.push_back({dj.get<std::string>(), 1.0});
        else
          a.devices.push_back({dj.value("id", ""), dj.value("share", 1.0)});
      }
    }
    if (sj.contains("precision")) {
      std::string pr = sj["precision"].get<std::string>();
      if (!parse_precision(pr, a.prec)) errors.push_back("strategy: 阶段 " + name + " 精度 '" + pr + "' 无效");
    }
    if (sj.contains("zero_copy")) a.zero_copy = sj["zero_copy"].get<bool>();
    if (sj.contains("assign")) a.dynamic = sj["assign"].get<std::string>() == "dynamic";
    for (auto& d : a.devices)
      if (d.second < 0) errors.push_back("strategy: 阶段 " + name + " 份额不能为负");
    return a;
  };
  for (const char* name : kStages) {
    json sj = st.contains(name) ? st[name] : json::object();
    s.stages[name] = parse_assign(sj, name, nullptr);
  }
  // 聚类阶段可细分：propagation（图传播 SpMM）/ assignment（簇分配 GEMM、正交化）
  for (const char* sub : {"propagation", "assignment"}) {
    std::string key = std::string("clustering.") + sub;
    json cj = st.value("clustering", json::object());
    if (cj.contains(sub))
      s.stages[key] = parse_assign(cj[sub], key, &s.stages["clustering"]);
    else if (st.contains(key))
      s.stages[key] = parse_assign(st[key], key, &s.stages["clustering"]);
  }
  return errors.size() == e0;
}

json Strategy::to_json() const {
  auto one = [](const StageAssign& a) {
    json devs = json::array();
    for (auto& d : a.devices) devs.push_back({{"id", d.first}, {"share", d.second}});
    return json{{"devices", devs},
                {"precision", precision_name(a.prec)},
                {"zero_copy", a.zero_copy},
                {"assign", a.dynamic ? "dynamic" : "static"}};
  };
  json st = json::object();
  for (auto& kv : stages)
    if (kv.first.find('.') == std::string::npos) st[kv.first] = one(kv.second);
  for (auto& kv : stages)
    if (kv.first.rfind("clustering.", 0) == 0) st["clustering"][kv.first.substr(11)] = one(kv.second);
  json pl = json::object();
  for (auto& kv : placement) pl[kv.first] = kv.second;
  json o = {{"name", name},
            {"dataset_location", dataset_location},
            {"placement", pl},
            {"partition", {{"num_chunks", num_chunks == 0 ? json("auto") : json(num_chunks)}, {"method", partition}}},
            {"pipeline", {{"overlap", overlap}, {"prefetch_depth", prefetch_depth}, {"halo_cache", halo_cache}}},
            {"stages", st},
            {"leader", leader}};
  if (!plugin.empty()) o["plugin"] = plugin;
  return o;
}

// ---------------------------------------------------------------------------
// 工作量模型
// ---------------------------------------------------------------------------
namespace {

double ebytes(Precision p) { return std::max(1.0, precision_bytes(p)); }

struct Shapes {
  double h = 64;      // 嵌入维
  double wbytes = 0;  // 权重字节（FP32 主副本）
  double flops_per_row = 0;
  double act_per_row = 0;
  double hidden_per_row = 0;
  double dom_k = 0, dom_n = 0;
};

Shapes shapes(const Workload& w) {
  Shapes s;
  std::vector<double> dims{w.ds.feature_dim};
  for (double x : w.alg.encoder_dims) dims.push_back(x);
  double best = -1;
  for (size_t i = 0; i + 1 < dims.size(); ++i) {
    double f = dims[i] * dims[i + 1];
    s.flops_per_row += 2 * f;
    s.act_per_row += dims[i + 1];
    s.wbytes += f * 4;
    if (i + 2 < dims.size()) s.hidden_per_row += dims[i + 1];
    if (f > best) {
      best = f;
      s.dom_k = dims[i];
      s.dom_n = dims[i + 1];
    }
  }
  s.h = dims.back();
  return s;
}

Kernel k_fe(const Workload& w, const Shapes& S, double Nc, Precision p, bool train, const std::string& wid) {
  Kernel k;
  k.stage = "feature_extraction";
  k.name = train ? "encoder_train" : "encoder_infer";
  k.op = OpClass::GEMM;
  k.prec = p;
  double eb = ebytes(p);
  double f = train ? w.alg.train_flops_factor : 1.0;
  k.flops = (S.flops_per_row + S.act_per_row) * Nc * f;
  k.seq_bytes = Nc * w.ds.feature_dim * w.ds.feature_bytes + Nc * S.h * eb + Nc * S.hidden_per_row * eb * 2;
  if (train) k.seq_bytes *= 2;
  k.gemm_m = Nc;
  k.gemm_k = S.dom_k;
  k.gemm_n = S.dom_n;
  k.weight_id = wid;
  k.weights_static = !train;
  return k;
}

double a_bytes(double Ec, double Nc, const Dataset& d) { return Ec * (d.index_bytes + 4) + (Nc + 1) * 8; }

Kernel k_gc_sort(const Workload& w, double Nc, double Ec, Precision p) {
  Kernel k;
  k.stage = "graph_construction";
  k.name = "csr_build";
  k.op = OpClass::SORT;
  k.prec = p;
  k.flops = 10 * Ec;
  double passes = 4;
  k.seq_bytes = passes * 2 * Ec * 2 * w.ds.index_bytes + a_bytes(Ec, Nc, w.ds);
  k.rand_accesses = 2 * Ec;
  k.rand_elem_bytes = 4;
  k.working_set = w.ds.nodes * 4;
  k.skew = w.ds.degree_skew;
  return k;
}

Kernel k_gc_knn(const Workload& w, const Shapes& S, double Nc, Precision p) {
  Kernel k;
  k.stage = "graph_construction";
  k.name = "knn_graph";
  k.op = OpClass::GEMM;
  k.prec = p;
  double eb = ebytes(p);
  double cand = w.ds.nodes * w.alg.knn_candidate_ratio;
  k.flops = 2 * Nc * cand * S.h + Nc * cand * std::log2(w.ds.knn_k + 1);
  k.seq_bytes = Nc * S.h * eb + w.ds.nodes * S.h * eb * w.alg.knn_candidate_ratio + Nc * w.ds.knn_k * 8 + Nc * 8;
  k.gemm_m = Nc;
  k.gemm_k = S.h;
  k.gemm_n = cand;
  k.weight_id = "Z_all";
  k.weights_static = true;
  return k;
}

double halo_rows(double N, double Nc, double Ec, double cut) {
  double other = N - Nc;
  if (other <= 0 || cut <= 0) return 0;
  double expect = cut * Ec;
  return other * (1 - std::exp(-expect / other));
}

Kernel k_spmm(const Workload& w, double Nc, double Ec, double hdim, Precision p, double halo) {
  Kernel k;
  k.stage = "clustering";
  k.name = "propagate_spmm";
  k.op = OpClass::SPMM;
  k.prec = p;
  double eb = ebytes(p);
  k.flops = 2 * Ec * hdim;
  k.seq_bytes = a_bytes(Ec, Nc, w.ds) + 2 * Nc * hdim * eb;
  k.rand_accesses = Ec;
  k.rand_elem_bytes = hdim * eb;
  k.working_set = (Nc + halo) * hdim * eb;
  k.skew = w.ds.degree_skew;
  k.halo_bytes = halo * hdim * eb;
  return k;
}

Kernel k_assign(double Nc, double hdim, double kc, Precision p, const std::string& wid) {
  Kernel k;
  k.stage = "clustering";
  k.name = "assign";
  k.op = OpClass::GEMM;
  k.prec = p;
  double eb = ebytes(p);
  k.flops = 2 * Nc * hdim * kc + 3 * Nc * kc + Nc * hdim;
  k.seq_bytes = Nc * hdim * eb + Nc * 4 + kc * hdim * 4 * 2;
  k.gemm_m = Nc;
  k.gemm_k = hdim;
  k.gemm_n = kc;
  k.weight_id = wid;
  k.weights_static = true;
  return k;
}

Kernel k_orth(double Nc, double b, double jav, Precision p) {
  Kernel k;
  k.stage = "clustering";
  k.name = "orthogonalize";
  k.op = OpClass::GEMM;
  k.prec = p;
  double eb = ebytes(p);
  k.flops = 4 * Nc * b * b * jav;
  k.seq_bytes = Nc * b * eb * (jav + 2);
  k.gemm_m = Nc;
  k.gemm_k = b;
  k.gemm_n = b * jav;
  k.weights_static = false;
  return k;
}

Kernel k_reduce(const std::string& stage, const std::string& name, double inputs_bytes, double flops) {
  Kernel k;
  k.stage = stage;
  k.name = name;
  k.op = OpClass::REDUCE;
  k.prec = Precision::FP32;
  k.flops = flops;
  k.seq_bytes = inputs_bytes;
  return k;
}

Kernel k_partition(const Dataset& d) {
  Kernel k;
  k.stage = "partition";
  k.name = "metis_partition";
  k.op = OpClass::SCATTER;
  k.prec = Precision::FP32;
  k.flops = 20 * d.edges;
  k.seq_bytes = 10 * d.edges * 2 * d.index_bytes;
  k.rand_accesses = 5 * d.edges;
  k.rand_elem_bytes = 8;
  k.working_set = d.nodes * 16;
  k.skew = d.degree_skew;
  return k;
}

double cut_ratio(const Workload& w, const std::string& method, int P) {
  if (P <= 1) return 0;
  double rnd = 1.0 - 1.0 / P;
  if (method == "random") return rnd;
  if (method == "metis") return std::min(rnd, w.ds.metis_cut_base * std::log2(static_cast<double>(P)));
  return rnd * (1 - w.ds.locality);
}

double clustering_edges(const Workload& w) { return w.ds.has_graph ? w.ds.edges : w.ds.nodes * w.ds.knn_k; }

}  // namespace

// 默认数据集位置：优先普通 SSD，其次 CSD，否则主机内存
std::string default_dataset_location(const System& sys) {
  for (const char* t : {"SSD", "CSD"})
    for (auto& d : sys.devices)
      if (d->type == t && d->mem) return d->id;
  return sys.host()->id;
}

namespace {

}  // namespace

// ---------------------------------------------------------------------------
std::vector<Kernel> stage_kernels(const Workload& w, const std::string& stage, Precision prec) {
  Shapes S = shapes(w);
  double N = w.ds.nodes;
  std::vector<Kernel> ks;
  if (stage == "feature_extraction") {
    for (int e = 0; e < w.alg.encoder_epochs; ++e) ks.push_back(k_fe(w, S, N, prec, true, "W@" + std::to_string(e)));
    ks.push_back(k_fe(w, S, N, prec, false, "W@final"));
  } else if (stage == "graph_construction") {
    if (w.ds.has_graph)
      ks.push_back(k_gc_sort(w, N, w.ds.edges, prec));
    else
      ks.push_back(k_gc_knn(w, S, N, prec));
  } else if (stage.rfind("clustering", 0) == 0) {
    bool prop = stage != "clustering.assignment";
    bool asg = stage != "clustering.propagation";
    double E = clustering_edges(w);
    int kc = w.ds.clusters;
    if (w.alg.method == "kmeans") {
      int hops_total = w.alg.prop_every_iter ? w.alg.prop_hops * w.alg.iterations : w.alg.prop_hops;
      if (prop)
        for (int i = 0; i < hops_total; ++i) ks.push_back(k_spmm(w, N, E, S.h, prec, 0));
      if (asg)
        for (int t = 0; t < w.alg.iterations; ++t) ks.push_back(k_assign(N, S.h, kc, prec, "C@" + std::to_string(t)));
    } else {
      double b = w.alg.lanczos_block;
      for (int t = 0; t < w.alg.iterations; ++t) {
        if (prop) ks.push_back(k_spmm(w, N, E, b, prec, 0));
        if (asg) ks.push_back(k_orth(N, b, (t + 1) / 2.0 + 1, prec));
      }
      if (asg)
        for (int t = 0; t < w.alg.kmeans_after; ++t) ks.push_back(k_assign(N, b, kc, prec, "C@" + std::to_string(t)));
    }
  }
  return ks;
}

int auto_chunks(const Workload& w, const Strategy& s, const System& sys) {
  Shapes S = shapes(w);
  double N = w.ds.nodes;
  double frac = s.overlap ? 1.0 / (s.prefetch_depth + 2.5) : 0.4;
  double E = clustering_edges(w);
  for (int P = 1; P <= 256; P *= 2) {
    bool ok = true;
    double Nc = N / P;
    for (auto it = s.stages.begin(); it != s.stages.end(); ++it) {
      double eb = ebytes(it->second.prec);
      double need = 0;
      std::string sn = it->first;
      if (sn == "feature_extraction")
        need = Nc * w.ds.feature_dim * w.ds.feature_bytes + Nc * S.h * eb + S.wbytes;
      else if (sn == "graph_construction")
        need = w.ds.has_graph ? (w.ds.edges / P) * 2 * w.ds.index_bytes + a_bytes(w.ds.edges / P, Nc, w.ds)
                              : Nc * S.h * eb * 3 + a_bytes(Nc * w.ds.knn_k, Nc, w.ds);
      else {
        double hdim = w.alg.method == "spectral" ? std::max<double>(w.alg.lanczos_block, S.h) : S.h;
        double cut = cut_ratio(w, s.partition, P);
        need = a_bytes(E / P, Nc, w.ds) + 2 * Nc * hdim * eb + halo_rows(N, Nc, E / P, cut) * hdim * eb;
      }
      for (auto& d : it->second.devices) {
        ComputeDevice* dev = sys.device(d.first);
        if (!dev || !dev->mem) continue;
        if (need > frac * dev->mem->capacity_bytes()) ok = false;
      }
    }
    if (ok) return P;
  }
  return 256;
}

json workload_characterize(const Workload& w) {
  json o = json::object();
  Shapes S = shapes(w);
  o["embedding_dim"] = S.h;
  o["feature_GB"] = w.ds.nodes * w.ds.feature_dim * w.ds.feature_bytes / GB;
  o["raw_graph_GB"] = w.ds.has_graph ? w.ds.edges * 2 * w.ds.index_bytes / GB : 0.0;
  o["csr_GB"] = a_bytes(clustering_edges(w), w.ds.nodes, w.ds) / GB;
  o["embedding_fp32_GB"] = w.ds.nodes * S.h * 4 / GB;
  json st = json::object();
  for (const char* s : kStages) {
    double fl = 0, by = 0, ra = 0;
    std::map<std::string, double> ops;
    for (auto& k : stage_kernels(w, s, Precision::FP32)) {
      fl += k.flops;
      by += k.seq_bytes + k.rand_accesses * k.rand_elem_bytes;
      ra += k.rand_accesses;
      ops[op_name(k.op)] += k.flops;
    }
    st[s] = {{"GFLOP", fl / 1e9},
             {"GB_accessed", by / GB},
             {"random_accesses_M", ra / 1e6},
             {"arith_intensity", by > 0 ? fl / by : 0.0},
             {"kernels", stage_kernels(w, s, Precision::FP32).size()}};
  }
  o["stages"] = st;
  return o;
}

// ---------------------------------------------------------------------------
// 任务图生成
// ---------------------------------------------------------------------------
bool build_task_graph(const Workload& w, const Strategy& s, const System& sys, TaskGraph& g,
                      std::vector<std::string>& errors) {
  size_t e0 = errors.size();
  g = TaskGraph();
  Shapes S = shapes(w);
  const std::string host = sys.host()->id;
  const std::string host_mem = sys.host()->mem->id;
  std::string leader = s.leader.empty() ? host : s.leader;
  ComputeDevice* ld = sys.device(leader);
  if (!ld || !ld->has_compute()) {
    errors.push_back("strategy: leader '" + leader + "' 不是可计算设备");
    return false;
  }
  // 数据集所在
  std::string dloc = s.dataset_location.empty() ? default_dataset_location(sys) : s.dataset_location;
  ComputeDevice* dl = sys.device(dloc);
  MemoryTier* dmem = nullptr;
  if (dl) dmem = dl->mem;
  if (!dmem) dmem = sys.memory(dloc);
  if (!dmem) {
    errors.push_back("strategy: dataset_location '" + dloc + "' 不是有效的设备或存储");
    return false;
  }
  auto resolve_mem = [&](const std::string& key, const std::string& def) -> std::string {
    auto it = s.placement.find(key);
    std::string v = it == s.placement.end() ? def : it->second;
    if (v.empty() || v == "home") return "";
    if (sys.memory(v)) return v;
    if (ComputeDevice* d = sys.device(v))
      if (d->mem) return d->mem->id;
    errors.push_back("strategy: placement." + key + " = '" + v + "' 不是有效存储");
    return "";
  };
  std::string feat_stage = resolve_mem("features", "home");
  std::string graph_stage = resolve_mem("graph", "home");
  std::string spill = resolve_mem("spill", host_mem);
  if (spill.empty()) spill = host_mem;
  if (errors.size() != e0) return false;

  // 阶段设备检查
  std::map<std::string, StageAssign> stg = s.stages;
  for (const char* st0 : kStages) stg[st0];
  for (auto& kv : stg) {
    const std::string& st = kv.first;
    auto& a = kv.second;
    if (a.devices.empty()) a.devices.push_back({host, 1.0});
    double tot = 0;
    for (auto& d : a.devices) {
      ComputeDevice* dev = sys.device(d.first);
      if (!dev) errors.push_back("strategy: 阶段 " + st + " 的设备 '" + d.first + "' 不存在");
      else if (!dev->has_compute()) errors.push_back("strategy: 阶段 " + st + " 的设备 '" + d.first + "' 没有计算能力");
      tot += d.second;
    }
    if (tot <= 0) errors.push_back("strategy: 阶段 " + st + " 的份额之和必须 > 0");
  }
  if (errors.size() != e0) return false;

  int P = s.num_chunks > 0 ? s.num_chunks : auto_chunks(w, s, sys);
  g.P = P;
  double N = w.ds.nodes, Nc = N / P;
  double E = clustering_edges(w), Ec = E / P;
  double cut = cut_ratio(w, s.partition, P);
  g.cut_ratio = cut;

  auto new_chunk = [&](const std::string& tensor, int idx, double bytes, const std::string& home) {
    Chunk c;
    c.id = static_cast<int>(g.chunks.size());
    c.tensor = tensor;
    c.name = idx >= 0 ? tensor + "#" + std::to_string(idx) : tensor;
    c.bytes = std::max(bytes, 1.0);
    c.home = home;
    g.chunks.push_back(c);
    return c.id;
  };
  auto new_task = [&](const std::string& name, const std::string& stage) -> Task& {
    Task t;
    t.id = static_cast<int>(g.tasks.size());
    t.name = name;
    t.stage = stage;
    g.tasks.push_back(t);
    return g.tasks.back();
  };
  auto out = [&](Task& t, int chunk) {
    t.outputs.push_back(chunk);
    g.chunks[chunk].producer = t.id;
  };
  auto in_full = [&](Task& t, int chunk) {
    TaskInput in;
    in.mode = TaskInput::FULL;
    in.chunk = chunk;
    t.inputs.push_back(in);
  };
  auto assign_dev = [&](Task& t, const StageAssign& a, int c) {
    t.zero_copy = a.zero_copy;
    if (a.dynamic) {
      t.dynamic = true;
      for (auto& d : a.devices)
        if (d.second > 0) t.candidates.push_back(d.first);
      return;
    }
    double tot = 0;
    for (auto& d : a.devices) tot += d.second;
    double pos = (c + 0.5) / P * tot, acc = 0;
    for (auto& d : a.devices) {
      acc += d.second;
      if (pos <= acc + 1e-12 && d.second > 0) {
        t.device = d.first;
        return;
      }
    }
    t.device = a.devices.back().first;
  };
  std::vector<int> prev_barrier_tasks;  // 无重叠时：上一阶段全部任务
  auto barrier = [&](const std::string& name, const std::vector<int>& tasks) -> int {
    Task& b = new_task("barrier:" + name, "barrier");
    b.kind = "barrier";
    b.deps = tasks;
    return b.id;
  };
  std::map<std::string, std::vector<int>> stage_tasks;

  // ---- 数据集原始分块 ----
  std::vector<int> X(P), G(P, -1);
  for (int c = 0; c < P; ++c) X[c] = new_chunk("X", c, Nc * w.ds.feature_dim * w.ds.feature_bytes, dmem->id);
  if (w.ds.has_graph)
    for (int c = 0; c < P; ++c) G[c] = new_chunk("G", c, Ec * 2 * w.ds.index_bytes, dmem->id);

  // ---- load：暂存 ----
  auto stage_in = [&](std::vector<int>& src, const std::string& dst, const std::string& tensor) {
    if (dst.empty() || dst == dmem->id) return;
    for (int c = 0; c < P; ++c) {
      if (src[c] < 0) continue;
      Task& t = new_task("stage_in " + tensor + "#" + std::to_string(c), "load");
      t.kind = "copy";
      t.copy_dst = dst;
      t.chunk = c;
      in_full(t, src[c]);
      int nc = new_chunk(tensor + "s", c, g.chunks[src[c]].bytes, dst);
      out(t, nc);
      stage_tasks["load"].push_back(t.id);
      src[c] = nc;
    }
  };
  stage_in(X, feat_stage, "X");
  if (w.ds.has_graph) stage_in(G, graph_stage, "G");
  int load_barrier = -1;
  if (!s.overlap && !stage_tasks["load"].empty()) load_barrier = barrier("load", stage_tasks["load"]);

  // ---- 特征提取 ----
  const StageAssign& afe = stg["feature_extraction"];
  int W = new_chunk("W@0", -1, S.wbytes, host_mem);
  for (int e = 0; e < w.alg.encoder_epochs; ++e) {
    std::vector<int> grads;
    std::vector<int> ep_tasks;
    for (int c = 0; c < P; ++c) {
      Task& t = new_task("fe_train e" + std::to_string(e) + " c" + std::to_string(c), "feature_extraction");
      t.k = k_fe(w, S, Nc, afe.prec, true, "W@" + std::to_string(e));
      t.chunk = c;
      t.iter = e;
      assign_dev(t, afe, c);
      in_full(t, X[c]);
      in_full(t, W);
      if (load_barrier >= 0) t.deps.push_back(load_barrier);
      int gr = new_chunk("grad@" + std::to_string(e), c, S.wbytes, spill);
      out(t, gr);
      grads.push_back(gr);
      ep_tasks.push_back(t.id);
      stage_tasks["feature_extraction"].push_back(t.id);
    }
    Task& sy = new_task("weight_sync e" + std::to_string(e), "feature_extraction");
    sy.k = k_reduce("feature_extraction", "weight_sync", S.wbytes * P, S.wbytes / 4 * P);
    sy.device = leader;
    sy.iter = e;
    for (int gr : grads) in_full(sy, gr);
    W = new_chunk("W@" + std::to_string(e + 1), -1, S.wbytes, spill);
    out(sy, W);
    stage_tasks["feature_extraction"].push_back(sy.id);
  }
  std::vector<int> Z(P);
  for (int c = 0; c < P; ++c) {
    Task& t = new_task("fe c" + std::to_string(c), "feature_extraction");
    t.k = k_fe(w, S, Nc, afe.prec, false, "W@final");
    t.chunk = c;
    assign_dev(t, afe, c);
    in_full(t, X[c]);
    in_full(t, W);
    if (load_barrier >= 0) t.deps.push_back(load_barrier);
    Z[c] = new_chunk("Z", c, Nc * S.h * ebytes(afe.prec), spill);
    out(t, Z[c]);
    stage_tasks["feature_extraction"].push_back(t.id);
  }
  int fe_barrier = -1;
  if (!s.overlap) fe_barrier = barrier("feature_extraction", stage_tasks["feature_extraction"]);

  // ---- 图划分（METIS 类，主机 CPU）----
  int perm = -1;
  if (s.partition == "metis" && P > 1 && w.ds.has_graph) {
    Task& t = new_task("metis_partition", "partition");
    t.k = k_partition(w.ds);
    t.device = host;
    for (int c = 0; c < P; ++c) in_full(t, G[c]);
    if (fe_barrier >= 0) t.deps.push_back(fe_barrier);
    perm = new_chunk("perm", -1, N * 4, spill);
    out(t, perm);
    stage_tasks["partition"].push_back(t.id);
  }

  // ---- 图构建 ----
  const StageAssign& agc = stg["graph_construction"];
  std::vector<int> A(P);
  for (int c = 0; c < P; ++c) {
    Task& t = new_task("gc c" + std::to_string(c), "graph_construction");
    t.chunk = c;
    assign_dev(t, agc, c);
    if (w.ds.has_graph) {
      t.k = k_gc_sort(w, Nc, Ec, agc.prec);
      in_full(t, G[c]);
      if (perm >= 0) in_full(t, perm);
    } else {
      t.k = k_gc_knn(w, S, Nc, agc.prec);
      in_full(t, Z[c]);
      if (P > 1) {
        TaskInput st;
        st.mode = TaskInput::HALO;
        for (int o = 0; o < P; ++o)
          if (o != c) st.group.push_back(Z[o]);
        st.bytes_per_member = g.chunks[Z[(c + 1) % P]].bytes * w.alg.knn_candidate_ratio;
        st.elem = -1;  // 负值表示流式（STREAM）输入：在 kernel 执行期间流式读入
        t.inputs.push_back(st);
      }
    }
    if (fe_barrier >= 0) t.deps.push_back(fe_barrier);
    if (load_barrier >= 0) t.deps.push_back(load_barrier);
    A[c] = new_chunk("A", c, a_bytes(Ec, Nc, w.ds), spill);
    out(t, A[c]);
    stage_tasks["graph_construction"].push_back(t.id);
  }
  int gc_barrier = -1;
  if (!s.overlap) {
    std::vector<int> both = stage_tasks["graph_construction"];
    for (int x : stage_tasks["partition"]) both.push_back(x);
    gc_barrier = barrier("graph_construction", both);
  }

  // ---- 聚类迭代 ----
  const StageAssign& aprop = stg.count("clustering.propagation") ? stg["clustering.propagation"] : stg["clustering"];
  const StageAssign& aasg = stg.count("clustering.assignment") ? stg["clustering.assignment"] : stg["clustering"];
  double ebc = ebytes(aprop.prec);
  int kc = w.ds.clusters;
  std::vector<int> H = Z;  // 当前嵌入分块
  double Hdim = S.h;
  int hop_id = 0;
  auto spmm_round = [&](const std::string& tag, double hdim) {
    std::vector<int> NH(P);
    double hr = halo_rows(N, Nc, Ec, cut);
    for (int c = 0; c < P; ++c) {
      Task& t = new_task("spmm " + tag + " c" + std::to_string(c), "clustering");
      t.k = k_spmm(w, Nc, Ec, hdim, aprop.prec, hr);
      t.chunk = c;
      t.iter = hop_id;
      assign_dev(t, aprop, c);
      in_full(t, A[c]);
      in_full(t, H[c]);
      if (P > 1 && hr > 0) {
        TaskInput hi;
        hi.mode = TaskInput::HALO;
        for (int o = 0; o < P; ++o)
          if (o != c) hi.group.push_back(H[o]);
        hi.elem = hdim * ebc;
        hi.bytes_per_member = hr * hdim * ebc / (P - 1);
        hi.accesses_per_member = cut * Ec / (P - 1);
        t.inputs.push_back(hi);
      }
      if (gc_barrier >= 0) t.deps.push_back(gc_barrier);
      NH[c] = new_chunk("H" + tag, c, Nc * hdim * ebc, spill);
      out(t, NH[c]);
      stage_tasks["clustering"].push_back(t.id);
    }
    H = NH;
    hop_id++;
  };
  int C = new_chunk("C@0", -1, kc * Hdim * 4, host_mem);
  std::vector<int> labels(P, -1);
  auto kmeans_iter = [&](int t_iter, double hdim, bool last) {
    std::vector<int> partial;
    std::string wid = "C@" + std::to_string(t_iter);
    for (int c = 0; c < P; ++c) {
      Task& t = new_task("assign t" + std::to_string(t_iter) + " c" + std::to_string(c), "clustering");
      t.k = k_assign(Nc, hdim, kc, aasg.prec, wid);
      t.chunk = c;
      t.iter = t_iter;
      assign_dev(t, aasg, c);
      in_full(t, H[c]);
      in_full(t, C);
      if (gc_barrier >= 0) t.deps.push_back(gc_barrier);
      int sp = new_chunk("S@" + std::to_string(t_iter), c, kc * (hdim + 1) * 4, spill);
      out(t, sp);
      partial.push_back(sp);
      if (last) {
        labels[c] = new_chunk("labels", c, Nc * 4, spill);
        out(t, labels[c]);
      }
      stage_tasks["clustering"].push_back(t.id);
    }
    Task& u = new_task("update t" + std::to_string(t_iter), "clustering");
    u.k = k_reduce("clustering", "centroid_update", kc * (hdim + 1) * 4 * P, kc * hdim * P);
    u.device = leader;
    u.iter = t_iter;
    for (int sp : partial) in_full(u, sp);
    C = new_chunk("C@" + std::to_string(t_iter + 1), -1, kc * hdim * 4, spill);
    out(u, C);
    stage_tasks["clustering"].push_back(u.id);
  };

  if (w.alg.method == "kmeans") {
    if (!w.alg.prop_every_iter)
      for (int l = 0; l < w.alg.prop_hops; ++l) spmm_round("p" + std::to_string(l), Hdim);
    for (int t = 0; t < w.alg.iterations; ++t) {
      if (w.alg.prop_every_iter)
        for (int l = 0; l < w.alg.prop_hops; ++l) spmm_round("t" + std::to_string(t) + "h" + std::to_string(l), Hdim);
      kmeans_iter(t, Hdim, t + 1 == w.alg.iterations);
    }
  } else {
    // 谱聚类：块 Lanczos 迭代（SpMM + 正交化 + 主节点小规模特征分解）
    double b = std::max(2, w.alg.lanczos_block);
    std::vector<int> V(P);
    for (int c = 0; c < P; ++c) V[c] = new_chunk("V0", c, Nc * b * ebc, spill);
    H = V;
    int Q = new_chunk("Q@0", -1, b * b * 4, host_mem);
    for (int t = 0; t < w.alg.iterations; ++t) {
      spmm_round("l" + std::to_string(t), b);
      std::vector<int> grams, NV(P);
      double jav = (t + 1) / 2.0 + 1;
      for (int c = 0; c < P; ++c) {
        Task& o = new_task("orth t" + std::to_string(t) + " c" + std::to_string(c), "clustering");
        o.k = k_orth(Nc, b, jav, aasg.prec);
        o.chunk = c;
        o.iter = t;
        assign_dev(o, aasg, c);
        in_full(o, H[c]);
        in_full(o, Q);
        int gm = new_chunk("gram@" + std::to_string(t), c, b * b * jav * 4, spill);
        out(o, gm);
        grams.push_back(gm);
        NV[c] = new_chunk("V" + std::to_string(t + 1), c, Nc * b * ebc, spill);
        out(o, NV[c]);
        stage_tasks["clustering"].push_back(o.id);
      }
      Task& u = new_task("eig t" + std::to_string(t), "clustering");
      double m = b * (t + 1);
      u.k = k_reduce("clustering", "small_eig", b * b * jav * 4 * P, 10 * m * m * m + b * b * jav * P);
      u.device = leader;
      u.iter = t;
      for (int gm : grams) in_full(u, gm);
      Q = new_chunk("Q@" + std::to_string(t + 1), -1, b * b * 4, spill);
      out(u, Q);
      stage_tasks["clustering"].push_back(u.id);
      H = NV;
    }
    Hdim = b;
    C = new_chunk("Ck@0", -1, kc * Hdim * 4, host_mem);
    int iters = std::max(1, w.alg.kmeans_after);
    for (int t = 0; t < iters; ++t) kmeans_iter(t, Hdim, t + 1 == iters);
  }

  // ---- 输出：标签汇集到主机 ----
  {
    Task& t = new_task("gather_labels", "output");
    t.k.name = "gather_labels";
    t.k.stage = "output";
    t.k.op = OpClass::SCAN;
    t.k.seq_bytes = N * 4;
    t.device = host;
    for (int l : labels)
      if (l >= 0) in_full(t, l);
    in_full(t, C);
    int res = new_chunk("result", -1, N * 4, host_mem);
    out(t, res);
    g.chunks[res].final_output = true;
  }

  // ---- 依赖：由数据依赖推导 ----
  for (auto& t : g.tasks) {
    for (auto& in : t.inputs) {
      if (in.mode == TaskInput::FULL) {
        int p = g.chunks[in.chunk].producer;
        if (p >= 0) t.deps.push_back(p);
      } else {
        for (int ch : in.group) {
          int p = g.chunks[ch].producer;
          if (p >= 0) t.deps.push_back(p);
        }
      }
    }
    std::sort(t.deps.begin(), t.deps.end());
    t.deps.erase(std::unique(t.deps.begin(), t.deps.end()), t.deps.end());
    t.deps.erase(std::remove(t.deps.begin(), t.deps.end(), t.id), t.deps.end());
  }

  // ---- 支持性检查 ----
  for (auto& t : g.tasks) {
    if (t.kind != "kernel") continue;
    std::vector<std::string> devs = t.dynamic ? t.candidates : std::vector<std::string>{t.device};
    std::vector<std::string> ok;
    std::string why;
    for (auto& d : devs) {
      ComputeDevice* dev = sys.device(d);
      std::string r;
      if (dev && dev->supports(t.k, &r) && (dev->peak_flops(t.k.prec, t.k.op) > 0 || t.k.flops == 0))
        ok.push_back(d);
      else if (why.empty())
        why = r.empty() ? std::string("设备 ") + d + " 不支持精度 " + precision_name(t.k.prec) : r;
    }
    if (ok.empty()) {
      errors.push_back("任务 '" + t.name + "' (" + op_name(t.k.op) + "/" + precision_name(t.k.prec) + ") 无可用设备: " + why);
      if (errors.size() - e0 > 5) break;
      continue;
    }
    if (t.dynamic) t.candidates = ok;
  }

  // ---- 摘要 ----
  double xb = 0, gb = 0, ab = 0, zb = 0;
  for (auto& c : g.chunks) {
    if (c.tensor == "X") xb += c.bytes;
    if (c.tensor == "G") gb += c.bytes;
    if (c.tensor == "A") ab += c.bytes;
    if (c.tensor == "Z") zb += c.bytes;
  }
  g.summary = {{"chunks", P},
               {"partition", s.partition},
               {"cut_ratio", cut},
               {"halo_rows_per_chunk", halo_rows(N, Nc, Ec, cut)},
               {"tasks", g.tasks.size()},
               {"tensor_chunks", g.chunks.size()},
               {"features_GB", xb / GB},
               {"raw_graph_GB", gb / GB},
               {"csr_GB", ab / GB},
               {"embeddings_GB", zb / GB},
               {"dataset_location", dmem->id},
               {"spill_tier", spill},
               {"leader", leader}};
  return errors.size() == e0;
}

}  // namespace hgc
