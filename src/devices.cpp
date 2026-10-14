#include <algorithm>
#include <cmath>

#include "hgcsim/device.hpp"

namespace hgc {

// ---------------------------------------------------------------------------
// 枚举辅助
// ---------------------------------------------------------------------------
const char* precision_name(Precision p) {
  switch (p) {
    case Precision::FP32: return "fp32";
    case Precision::FP16: return "fp16";
    case Precision::BF16: return "bf16";
    case Precision::FP8: return "fp8";
    case Precision::INT8: return "int8";
    case Precision::INT4: return "int4";
  }
  return "?";
}
bool parse_precision(const std::string& s, Precision& out) {
  if (s == "fp32") out = Precision::FP32;
  else if (s == "fp16") out = Precision::FP16;
  else if (s == "bf16") out = Precision::BF16;
  else if (s == "fp8") out = Precision::FP8;
  else if (s == "int8") out = Precision::INT8;
  else if (s == "int4") out = Precision::INT4;
  else return false;
  return true;
}
double precision_bytes(Precision p) {
  switch (p) {
    case Precision::FP32: return 4;
    case Precision::FP16:
    case Precision::BF16: return 2;
    case Precision::FP8:
    case Precision::INT8: return 1;
    case Precision::INT4: return 0.5;
  }
  return 4;
}
int precision_bits(Precision p) { return static_cast<int>(precision_bytes(p) * 8); }
const char* op_name(OpClass o) {
  switch (o) {
    case OpClass::GEMM: return "GEMM";
    case OpClass::SPMM: return "SpMM";
    case OpClass::SORT: return "SORT";
    case OpClass::SCATTER: return "SCATTER";
    case OpClass::ELEMWISE: return "ELEMWISE";
    case OpClass::REDUCE: return "REDUCE";
    case OpClass::TOPK: return "TOPK";
    case OpClass::SCAN: return "SCAN";
  }
  return "?";
}

static bool is_float(Precision p) {
  return p == Precision::FP32 || p == Precision::FP16 || p == Precision::BF16 || p == Precision::FP8;
}
static double tile_eff(double x, double t) {
  if (x <= 0) return 1.0;
  return x / (std::ceil(x / t) * t);
}

// ---------------------------------------------------------------------------
// 基类
// ---------------------------------------------------------------------------
ComputeDevice::ComputeDevice(std::string id_, std::string type_) : id(std::move(id_)), type(std::move(type_)) {}

void ComputeDevice::add_common(double launch_us, double link_gen, double lanes, double cxl_ver, double pj_flop) {
  p.add("launch_overhead_us", launch_us, 0.1, 10000, "us", "kernel 启动/调度开销");
  p.add("max_link_gen", link_gen, 1, 7, "", "设备侧支持的最高 PCIe 代数", true);
  p.add("max_lanes", lanes, 1, 16, "", "设备侧最大通道数", true, {1, 2, 4, 8, 16});
  p.add("cxl_max_version", cxl_ver, 0, 4.0, "", "设备支持的最高 CXL 版本（0 = 不支持 CXL）", false,
        {0, 1.1, 2.0, 3.0, 3.1, 3.2, 4.0});
  p.add("pj_per_flop", pj_flop, 0, 500, "pJ", "FP32 单次运算能耗（低精度按位宽线性缩放；纯存储设备为 0）");
}

static double max_tdp_for(const std::string& type) {
  if (type == "CPU") return 600;
  if (type == "GPU") return 2500;
  if (type == "NPU") return 1500;
  if (type == "TPU") return 1500;
  if (type == "FPGA") return 400;
  if (type == "NMP") return 800;
  if (type == "CSD") return 75;
  if (type == "CIM_RRAM" || type == "CIM_MRAM") return 300;
  return 1e9;
}

void ComputeDevice::validate(std::vector<std::string>& errors) const {
  double mt = max_tdp_for(type);
  double t = tdp_w();
  if (t > mt)
    errors.push_back("设备 '" + id + "': 推导功耗 " + ParamSet::fmt(std::round(t)) + " W 超过 " + type +
                     " 类器件功耗上界 " + ParamSet::fmt(mt) + " W（散热/供电约束），请降低规模或频率");
}

double ComputeDevice::peak_flops(Precision, OpClass) const { return 0; }
bool ComputeDevice::supports(const Kernel&, std::string*) const { return true; }
double ComputeDevice::op_efficiency(OpClass) const { return 0.7; }
double ComputeDevice::shape_efficiency(const Kernel&) const { return 1.0; }
double ComputeDevice::pj_per_flop(Precision prec) const {
  return p.get("pj_per_flop") * precision_bits(prec) / 32.0;
}

KernelCost ComputeDevice::roofline(const Kernel& k, double peak) const {
  KernelCost c;
  c.peak_flops = peak;
  double eff = op_efficiency(k.op) * shape_efficiency(k);
  eff = std::max(eff, 1e-4);
  c.compute_s = (k.flops > 0 && peak > 0) ? k.flops / (peak * eff) : 0;
  double bw = mem ? mem->seq_bw() : 1e11;
  c.mem_s = k.seq_bytes / bw;
  double moved = k.seq_bytes;
  if (k.rand_accesses > 0) {
    double cache = cache_bytes();
    double hit;
    if (k.working_set <= 0 || k.working_set <= cache)
      hit = 0.95;
    else
      hit = std::min(0.95, std::pow(cache / k.working_set, 1.0 - std::min(k.skew, 0.95)));
    double miss = k.rand_accesses * (1 - hit);
    double elem = std::max(k.rand_elem_bytes, 1.0);
    double ubw = 1e10;
    if (mem) {
      ubw = mem->random_useful_bw(elem, outstanding()) * random_boost();
      ubw = std::min(ubw, mem->peak_bw() / mem->amplification(elem));
      moved += miss * elem * mem->amplification(elem);
    }
    c.mem_s += miss * elem / ubw + k.rand_accesses * hit * elem / cache_bw();
  }
  c.setup_s = launch_s();
  c.time_s = c.setup_s + std::max(c.compute_s, c.mem_s) + overlap_penalty() * std::min(c.compute_s, c.mem_s);
  double mem_pj = mem ? mem->pj_per_bit() : 10;
  c.energy_j = k.flops * pj_per_flop(k.prec) * 1e-12 + moved * 8 * mem_pj * 1e-12;
  return c;
}

KernelCost ComputeDevice::estimate(const Kernel& k, bool) {
  KernelCost c;
  std::string why;
  if (!supports(k, &why)) {
    c.supported = false;
    c.reason = why;
    return c;
  }
  double peak = peak_flops(k.prec, k.op);
  if (peak <= 0 && k.flops > 0) {
    c.supported = false;
    c.reason = std::string("设备 ") + id + " 不支持精度 " + precision_name(k.prec);
    return c;
  }
  return roofline(k, peak);
}

json ComputeDevice::describe_derived() const {
  json o;
  o["id"] = id;
  o["type"] = type;
  json pk = json::object();
  if (has_compute()) {
    for (Precision pr : {Precision::FP32, Precision::FP16, Precision::BF16, Precision::FP8, Precision::INT8,
                         Precision::INT4}) {
      double g = peak_flops(pr, OpClass::GEMM);
      double v = peak_flops(pr, OpClass::ELEMWISE);
      if (g > 0 || v > 0) pk[precision_name(pr)] = {{"matrix_TFLOPS", g / 1e12}, {"vector_TFLOPS", v / 1e12}};
    }
  }
  o["peak"] = pk;
  o["cache_MB"] = cache_bytes() / MB;
  o["outstanding_requests"] = outstanding();
  o["tdp_w"] = tdp_w();
  o["cost_usd"] = cost_usd();
  if (mem) o["memory"] = mem->describe_derived();
  return o;
}

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------
CPUDevice::CPUDevice(const std::string& id_) : ComputeDevice(id_, "CPU") {
  // 默认: 8 核 4.0GHz AVX2 双 FMA 的桌面处理器（≈1 TFLOPS FP32）
  p.add("cores", 8, 1, 384, "个", "物理核数（单路最高 192，双路 384）", true);
  p.add("freq_ghz", 4.0, 0.8, 6.0, "GHz", "全核频率");
  p.add("simd_bits", 256, 128, 512, "bit", "SIMD 宽度 (SSE/AVX2/AVX-512)", true, {128, 256, 512});
  p.add("fma_units", 2, 1, 4, "个", "每核 FMA 单元数", true);
  p.add("amx", 0, 0, 1, "", "是否具备 AMX 矩阵扩展 (BF16/INT8)", true);
  p.add("llc_mb", 32, 1, 1152, "MB", "末级缓存容量（3D V-Cache 最高 1152MB）");
  p.add("lfb_per_core", 12, 4, 64, "个", "每核可并发的未命中请求数");
  add_common(2, 5, 16, 0, 20);
}

double CPUDevice::peak_flops(Precision prec, OpClass op) const {
  double c = p.get("cores"), f = p.get("freq_ghz") * 1e9;
  double fp32 = c * f * p.get("fma_units") * (p.get("simd_bits") / 32.0) * 2.0;
  bool amx = p.getb("amx") && op == OpClass::GEMM;
  switch (prec) {
    case Precision::FP32: return fp32;
    case Precision::FP16:
    case Precision::BF16: return amx ? c * f * 1024.0 : fp32 * (p.geti("simd_bits") == 512 ? 2.0 : 1.0);
    case Precision::INT8: return amx ? c * f * 2048.0 : fp32 * 4.0;
    default: return 0;
  }
}
bool CPUDevice::supports(const Kernel& k, std::string* why) const {
  if (k.prec == Precision::FP8 || k.prec == Precision::INT4) {
    if (why) *why = std::string("CPU 不支持 ") + precision_name(k.prec) + " 运算";
    return false;
  }
  return true;
}
double CPUDevice::op_efficiency(OpClass op) const {
  switch (op) {
    case OpClass::GEMM: return 0.85;
    case OpClass::SPMM: return 0.6;
    case OpClass::SORT: return 0.6;
    case OpClass::SCATTER: return 0.5;
    case OpClass::TOPK: return 0.6;
    default: return 0.8;
  }
}
double CPUDevice::cache_bw() const { return p.get("cores") * 32.0 * p.get("freq_ghz") * 1e9; }
double CPUDevice::tdp_w() const {
  double f = p.get("freq_ghz");
  return 30 + p.get("cores") * f * f * 0.17 * std::pow(p.get("simd_bits") / 256.0, 0.25) * (p.getb("amx") ? 1.1 : 1.0);
}
double CPUDevice::cost_usd() const {
  return 30 + 15 * std::pow(p.get("cores"), 1.3) * std::pow(p.get("freq_ghz") / 3.0, 1.5) * (p.getb("amx") ? 1.15 : 1.0) +
         p.get("llc_mb") * 2.0;
}

// ---------------------------------------------------------------------------
// GPU
// ---------------------------------------------------------------------------
GPUDevice::GPUDevice(const std::string& id_) : ComputeDevice(id_, "GPU") {
  // 默认: 消费级独显（46 SM @2.475GHz, ≈29 TFLOPS FP32, ≈117 TFLOPS FP16 Tensor）
  p.add("sms", 46, 1, 256, "个", "流多处理器数 (Rubin: 224)", true);
  p.add("freq_ghz", 2.475, 0.5, 3.0, "GHz", "加速频率");
  p.add("fp32_flops_per_sm_cycle", 256, 64, 512, "FLOP", "每 SM 每周期 FP32 运算数", true, {64, 128, 256, 512});
  p.add("tensor_fp16_flops_per_sm_cycle", 1024, 0, 32768, "FLOP", "每 SM 每周期 FP16 Tensor 稠密运算数 (B200≈8192)");
  p.add("fp8_ratio", 2, 0, 2, "×", "FP8 相对 FP16 Tensor 吞吐倍数 (0=不支持)");
  p.add("int8_ratio", 2, 0, 2, "×", "INT8 相对 FP16 Tensor 吞吐倍数");
  p.add("int4_ratio", 4, 0, 4, "×", "INT4 相对 FP16 Tensor 吞吐倍数 (0=不支持)");
  p.add("l2_mb", 36, 0.5, 512, "MB", "L2 缓存容量");
  p.add("outstanding_per_sm", 256, 16, 2048, "个", "每 SM 可并发访存请求");
  add_common(5, 4, 16, 0, 6);
}
double GPUDevice::peak_flops(Precision prec, OpClass op) const {
  double s = p.get("sms"), f = p.get("freq_ghz") * 1e9;
  double fp32 = s * f * p.get("fp32_flops_per_sm_cycle");
  double tc = s * f * p.get("tensor_fp16_flops_per_sm_cycle");
  if (op == OpClass::GEMM) {
    switch (prec) {
      case Precision::FP32: return fp32;
      case Precision::FP16:
      case Precision::BF16: return std::max(tc, fp32);
      case Precision::FP8: return tc * p.get("fp8_ratio");
      case Precision::INT8: return tc * p.get("int8_ratio");
      case Precision::INT4: return tc * p.get("int4_ratio");
    }
  }
  switch (prec) {
    case Precision::FP32: return fp32;
    case Precision::INT4: return p.get("int4_ratio") > 0 ? fp32 * 2 : 0;
    default: return fp32 * 2;  // half2 / 打包整数
  }
}
bool GPUDevice::supports(const Kernel& k, std::string* why) const {
  if (k.prec == Precision::FP8 && p.get("fp8_ratio") <= 0) {
    if (why) *why = "该 GPU 不支持 FP8";
    return false;
  }
  if (k.prec == Precision::INT4 && p.get("int4_ratio") <= 0) {
    if (why) *why = "该 GPU 不支持 INT4";
    return false;
  }
  return true;
}
double GPUDevice::op_efficiency(OpClass op) const {
  switch (op) {
    case OpClass::GEMM: return 0.75;
    case OpClass::SPMM: return 0.5;
    case OpClass::SORT: return 0.5;
    case OpClass::SCATTER: return 0.4;
    case OpClass::TOPK: return 0.5;
    case OpClass::REDUCE: return 0.7;
    default: return 0.8;
  }
}
double GPUDevice::shape_efficiency(const Kernel& k) const {
  if (k.op != OpClass::GEMM) return 1.0;
  return std::max(0.03, tile_eff(k.gemm_n, 64) * tile_eff(k.gemm_k, 32));
}
double GPUDevice::cache_bw() const { return p.get("sms") * 64.0 * p.get("freq_ghz") * 1e9; }
double GPUDevice::tdp_w() const {
  double f = p.get("freq_ghz");
  double tf = peak_flops(Precision::FP16, OpClass::GEMM) / 1e12;
  return 30 + 0.3 * p.get("sms") * f * f + 2.2 * std::pow(tf, 0.75);
}
double GPUDevice::cost_usd() const {
  double tf = peak_flops(Precision::FP16, OpClass::GEMM) / 1e12;
  return 200 + 6 * std::pow(tf, 0.9) + p.get("l2_mb") * 3;
}

// ---------------------------------------------------------------------------
// NPU（昇腾类：Cube 矩阵单元 + Vector 单元）
// ---------------------------------------------------------------------------
NPUDevice::NPUDevice(const std::string& id_) : ComputeDevice(id_, "NPU") {
  // 默认模板: 32 AI Core × 4096 MAC (16×16×16 Cube) @1.5GHz ≈ 393 TFLOPS FP16
  p.add("ai_cores", 32, 1, 128, "个", "AI Core 数", true);
  p.add("freq_ghz", 1.5, 0.5, 2.5, "GHz", "频率");
  p.add("cube_macs_per_core", 4096, 256, 16384, "MAC", "每核 Cube 单元每周期 MAC 数", true);
  p.add("vector_flops_per_core_cycle", 256, 32, 4096, "FLOP", "每核向量单元 FP16 每周期运算数");
  p.add("int8_ratio", 2, 0, 2, "×", "INT8 相对 FP16 Cube 吞吐");
  p.add("fp8_ratio", 0, 0, 2, "×", "FP8 相对 FP16 Cube 吞吐 (0=不支持)");
  p.add("buffer_mb", 64, 1, 512, "MB", "片上 L2/Buffer 容量");
  p.add("outstanding_per_core", 128, 16, 1024, "个", "每核可并发访存请求");
  p.add("gather_efficiency", 0.3, 0.05, 1.0, "", "不规则 gather/scatter 访存效率（DMA 引擎面向块访问）");
  add_common(20, 5, 16, 0, 2.0);
}
double NPUDevice::peak_flops(Precision prec, OpClass op) const {
  double c = p.get("ai_cores"), f = p.get("freq_ghz") * 1e9;
  double cube = c * f * p.get("cube_macs_per_core") * 2;
  double vec = c * f * p.get("vector_flops_per_core_cycle");
  if (op == OpClass::GEMM) {
    switch (prec) {
      case Precision::FP16:
      case Precision::BF16: return cube;
      case Precision::INT8: return cube * p.get("int8_ratio");
      case Precision::FP8: return cube * p.get("fp8_ratio");
      case Precision::FP32: return vec / 2;  // Cube 不支持 FP32，退化到向量单元
      default: return 0;
    }
  }
  switch (prec) {
    case Precision::FP32: return vec / 2;
    case Precision::INT4: return 0;
    default: return vec;
  }
}
bool NPUDevice::supports(const Kernel& k, std::string* why) const {
  if (k.prec == Precision::INT4 || (k.prec == Precision::FP8 && p.get("fp8_ratio") <= 0)) {
    if (why) *why = std::string("NPU 不支持 ") + precision_name(k.prec);
    return false;
  }
  return true;
}
double NPUDevice::op_efficiency(OpClass op) const {
  switch (op) {
    case OpClass::GEMM: return 0.7;
    case OpClass::SPMM: return 0.3;
    case OpClass::SORT: return 0.2;
    case OpClass::SCATTER: return 0.2;
    case OpClass::TOPK: return 0.4;
    case OpClass::REDUCE: return 0.6;
    default: return 0.7;
  }
}
double NPUDevice::shape_efficiency(const Kernel& k) const {
  if (k.op != OpClass::GEMM) return 1.0;
  return std::max(0.03, tile_eff(k.gemm_n, 16) * tile_eff(k.gemm_k, 16));
}
double NPUDevice::cache_bw() const { return p.get("ai_cores") * 128.0 * p.get("freq_ghz") * 1e9; }
double NPUDevice::tdp_w() const { return 40 + 0.8 * peak_flops(Precision::FP16, OpClass::GEMM) / 1e12; }
double NPUDevice::cost_usd() const {
  return 300 + 8 * std::pow(peak_flops(Precision::FP16, OpClass::GEMM) / 1e12, 0.9) + p.get("buffer_mb") * 4;
}

// ---------------------------------------------------------------------------
// TPU（脉动阵列 MXU）
// ---------------------------------------------------------------------------
TPUDevice::TPUDevice(const std::string& id_) : ComputeDevice(id_, "TPU") {
  // 默认模板: 4 × 128×128 MXU @1.5GHz ≈ 197 TFLOPS BF16（v5e 量级）
  p.add("mxu_count", 4, 1, 32, "个", "矩阵乘单元 MXU 数", true);
  p.add("mxu_dim", 128, 64, 256, "", "脉动阵列边长", true, {64, 128, 256});
  p.add("freq_ghz", 1.5, 0.5, 2.5, "GHz", "频率");
  p.add("vpu_flops_per_cycle", 4096, 256, 65536, "FLOP", "向量单元每周期 FP32 运算数");
  p.add("sparsecores", 0, 0, 8, "个", "SparseCore 数（加速 gather/scatter 与嵌入查找）", true);
  p.add("fp8_ratio", 0, 0, 2, "×", "FP8 相对 BF16 吞吐 (0=不支持)");
  p.add("int8_ratio", 2, 0, 2, "×", "INT8 相对 BF16 吞吐");
  p.add("fp32_matmul_ratio", 0.17, 0.05, 1.0, "×", "FP32 矩阵乘（多遍 BF16 模拟）相对 BF16 吞吐");
  p.add("vmem_mb", 128, 16, 512, "MB", "片上向量存储/CMEM 容量");
  p.add("outstanding", 1024, 64, 8192, "个", "可并发访存请求");
  add_common(10, 4, 16, 0, 1.5);
}
double TPUDevice::peak_flops(Precision prec, OpClass op) const {
  double d = p.get("mxu_dim");
  double mxu = p.get("mxu_count") * d * d * 2 * p.get("freq_ghz") * 1e9;
  double vpu = p.get("vpu_flops_per_cycle") * p.get("freq_ghz") * 1e9;
  if (op == OpClass::GEMM) {
    switch (prec) {
      case Precision::BF16:
      case Precision::FP16: return mxu;
      case Precision::FP8: return mxu * p.get("fp8_ratio");
      case Precision::INT8: return mxu * p.get("int8_ratio");
      case Precision::FP32: return mxu * p.get("fp32_matmul_ratio");
      default: return 0;
    }
  }
  if (prec == Precision::INT4) return 0;
  return prec == Precision::FP32 ? vpu : vpu * 2;
}
bool TPUDevice::supports(const Kernel& k, std::string* why) const {
  if (k.prec == Precision::INT4 || (k.prec == Precision::FP8 && p.get("fp8_ratio") <= 0)) {
    if (why) *why = std::string("TPU 不支持 ") + precision_name(k.prec);
    return false;
  }
  return true;
}
double TPUDevice::op_efficiency(OpClass op) const {
  bool sc = p.geti("sparsecores") > 0;
  switch (op) {
    case OpClass::GEMM: return 0.8;
    case OpClass::SPMM: return sc ? 0.5 : 0.2;
    case OpClass::SORT: return 0.15;
    case OpClass::SCATTER: return sc ? 0.5 : 0.15;
    case OpClass::TOPK: return 0.4;
    case OpClass::REDUCE: return 0.6;
    default: return 0.7;
  }
}
double TPUDevice::shape_efficiency(const Kernel& k) const {
  if (k.op != OpClass::GEMM) return 1.0;
  double d = p.get("mxu_dim");
  return std::max(0.01, tile_eff(k.gemm_n, d) * tile_eff(k.gemm_k, d));
}
double TPUDevice::random_boost() const {
  int sc = p.geti("sparsecores");
  return sc == 0 ? 0.15 : std::min(1.0, 0.15 + 0.25 * sc);
}
double TPUDevice::cache_bw() const { return p.get("vpu_flops_per_cycle") * 4 * p.get("freq_ghz") * 1e9; }
double TPUDevice::tdp_w() const { return 30 + 0.45 * peak_flops(Precision::BF16, OpClass::GEMM) / 1e12 + 15 * p.get("sparsecores"); }
double TPUDevice::cost_usd() const {
  return 300 + 5 * std::pow(peak_flops(Precision::BF16, OpClass::GEMM) / 1e12, 0.9) + 250 * p.get("sparsecores");
}

// ---------------------------------------------------------------------------
// FPGA
// ---------------------------------------------------------------------------
FPGADevice::FPGADevice(const std::string& id_) : ComputeDevice(id_, "FPGA") {
  // 默认模板: Alveo V80 量级（10848 DSP，片上 ≈84MB）
  p.add("dsp", 10848, 100, 20000, "个", "DSP 数", true);
  p.add("freq_mhz", 400, 100, 800, "MHz", "用户逻辑频率");
  p.add("dsp_utilization", 0.7, 0.1, 0.9, "", "可布线的 DSP 利用率上限");
  p.add("dsp_per_fp32_mac", 1, 0.5, 5, "", "每个 FP32 MAC 消耗 DSP 数");
  p.add("dsp_per_fp16_mac", 1, 0.5, 4, "", "每个 FP16 MAC 消耗 DSP 数");
  p.add("int8_macs_per_dsp", 2, 1, 4, "", "每个 DSP 可打包的 INT8 MAC 数");
  p.add("lut_k", 2600, 100, 10000, "K", "LUT 数量（千）");
  p.add("onchip_mb", 84, 1, 200, "MB", "片上 BRAM+URAM 容量");
  p.add("max_resident_kernels", 2, 1, 8, "个", "可同时驻留的内核数（受 LUT 约束）", true);
  p.add("reconfig_ms", 150, 1, 5000, "ms", "（部分）重配置时间");
  p.add("outstanding", 4096, 64, 16384, "个", "定制访存引擎可并发请求");
  add_common(10, 4, 16, 0, 2.5);
}
void FPGADevice::validate(std::vector<std::string>& errors) const {
  ComputeDevice::validate(errors);
  int lim = static_cast<int>(std::floor(p.get("lut_k") / 600.0));
  if (p.geti("max_resident_kernels") > std::max(1, lim))
    errors.push_back("设备 '" + id + "': 驻留内核数超过 LUT 资源允许值 " + std::to_string(std::max(1, lim)) +
                     "（每个内核约 600K LUT）");
}
double FPGADevice::peak_flops(Precision prec, OpClass) const {
  double base = p.get("dsp") * p.get("dsp_utilization") * p.get("freq_mhz") * 1e6 * 2;
  switch (prec) {
    case Precision::FP32: return base / p.get("dsp_per_fp32_mac");
    case Precision::FP16:
    case Precision::BF16: return base / p.get("dsp_per_fp16_mac");
    case Precision::FP8:
    case Precision::INT8: return base * p.get("int8_macs_per_dsp");
    case Precision::INT4: return base * p.get("int8_macs_per_dsp") * 2;
  }
  return 0;
}
bool FPGADevice::supports(const Kernel&, std::string*) const { return true; }
double FPGADevice::op_efficiency(OpClass op) const {
  switch (op) {
    case OpClass::GEMM: return 0.85;
    case OpClass::SCAN: return 0.95;
    default: return 0.9;
  }
}
// 片上 BRAM/URAM 端口众多，聚合带宽约与容量成正比（V80 量级 ≈20 TB/s）
double FPGADevice::cache_bw() const { return 20e12 * p.get("onchip_mb") / 84.0 * p.get("freq_mhz") / 400.0; }
KernelCost FPGADevice::estimate(const Kernel& k, bool commit) {
  KernelCost c = ComputeDevice::estimate(k, commit);
  if (!c.supported) return c;
  bool loaded = std::find(loaded_.begin(), loaded_.end(), k.op) != loaded_.end();
  if (!loaded) {
    c.extra_s = p.get("reconfig_ms") * MS;
    c.time_s += c.extra_s;
    if (commit) {
      loaded_.push_back(k.op);
      while (static_cast<int>(loaded_.size()) > p.geti("max_resident_kernels")) loaded_.pop_front();
    }
  } else if (commit) {
    loaded_.erase(std::find(loaded_.begin(), loaded_.end(), k.op));
    loaded_.push_back(k.op);
  }
  return c;
}
double FPGADevice::tdp_w() const {
  return 25 + p.get("dsp") * p.get("dsp_utilization") * p.get("freq_mhz") / 1000.0 * 0.03 + p.get("lut_k") * 0.005;
}
double FPGADevice::cost_usd() const { return 500 + p.get("dsp") * 0.6 + p.get("lut_k") * 1.5; }

// ---------------------------------------------------------------------------
// NMP / PIM
// ---------------------------------------------------------------------------
NMPDevice::NMPDevice(const std::string& id_) : ComputeDevice(id_, "NMP") {
  // 默认模板: UPMEM 量级（2560 DPU @350MHz, 每 DPU 64MB / 0.7 GB/s）
  p.add("units", 2560, 1, 8192, "个", "近存处理单元数（DPU/PCU）", true);
  p.add("unit_freq_mhz", 350, 100, 1500, "MHz", "处理单元频率");
  p.add("int_ops_per_cycle_per_unit", 0.17, 0.01, 64, "op", "每单元每周期整数运算（UPMEM 加法≈0.17）");
  p.add("fp_ops_per_cycle_per_unit", 0.02, 0.001, 64, "op", "每单元每周期浮点运算（UPMEM 软件模拟≈0.02，HBM-PIM 16 路 SIMD）");
  p.add("unit_bw_gbps", 0.7, 0.1, 64, "GB/s", "每单元内部存储带宽");
  p.add("mb_per_unit", 64, 8, 4096, "MB", "每单元存储容量");
  p.add("inter_unit_via_host", 1, 0, 1, "", "单元间通信是否需经主机中转（UPMEM=1）", true);
  p.add("inter_unit_bw_gbps", 50, 1, 2000, "GB/s", "单元间片上网络带宽（不经主机时）");
  p.add("host_xfer_efficiency", 0.4, 0.05, 1.0, "", "主机↔近存数据传输效率（数据重排/转置开销）");
  p.add("unit_latency_ns", 100, 10, 1000, "ns", "单元本地访存延迟");
  p.add("outstanding_per_unit", 4, 1, 64, "个", "每单元并发访存请求");
  p.add("pj_per_bit_internal", 1.5, 0.1, 20, "pJ/bit", "单元内部访存能耗");
  add_common(100, 4, 16, 0, 3.0);
}
void NMPDevice::validate(std::vector<std::string>& errors) const {
  ComputeDevice::validate(errors);
  if (mem) {
    double cap = p.get("units") * p.get("mb_per_unit") * MB;
    if (std::fabs(mem->capacity_bytes() - cap) > 0.01 * cap)
      errors.push_back("设备 '" + id + "': 近存设备存储容量必须等于 units × mb_per_unit = " +
                       ParamSet::fmt(cap / GB) + " GB（当前 " + ParamSet::fmt(mem->capacity_bytes() / GB) + " GB）");
  }
}
double NMPDevice::peak_flops(Precision prec, OpClass) const {
  double base = p.get("units") * p.get("unit_freq_mhz") * 1e6;
  if (is_float(prec)) return base * p.get("fp_ops_per_cycle_per_unit") * (prec == Precision::FP32 ? 1 : 2);
  return base * p.get("int_ops_per_cycle_per_unit") * (prec == Precision::INT4 ? 2 : 1);
}
bool NMPDevice::supports(const Kernel&, std::string*) const { return true; }
double NMPDevice::op_efficiency(OpClass op) const {
  switch (op) {
    case OpClass::GEMM: return 0.6;
    case OpClass::SORT: return 0.5;
    default: return 0.85;
  }
}
KernelCost NMPDevice::estimate(const Kernel& k, bool) {
  KernelCost c;
  double peak = peak_flops(k.prec, k.op);
  c.peak_flops = peak;
  c.compute_s = k.flops > 0 ? k.flops / (peak * op_efficiency(k.op)) : 0;
  double ibw = internal_bw() * 0.9;
  c.mem_s = k.seq_bytes / ibw;
  double moved = k.seq_bytes;
  if (k.rand_accesses > 0) {
    double elem = std::max(k.rand_elem_bytes, 8.0);
    double bytes = k.rand_accesses * std::ceil(elem / 8.0) * 8.0;
    double t_bw = bytes / (ibw * 0.8);
    double t_lat = k.rand_accesses * p.get("unit_latency_ns") * NS / (p.get("units") * p.get("outstanding_per_unit"));
    c.mem_s += std::max(t_bw, t_lat);
    moved += bytes;
  }
  if (k.halo_bytes > 0) {
    if (p.getb("inter_unit_via_host"))
      c.extra_s = 2 * k.halo_bytes / (std::max(attach_bw, 1e8) * p.get("host_xfer_efficiency"));
    else
      c.extra_s = k.halo_bytes / (p.get("inter_unit_bw_gbps") * GBps);
  }
  c.setup_s = launch_s();
  c.time_s = c.setup_s + std::max(c.compute_s, c.mem_s) + 0.05 * std::min(c.compute_s, c.mem_s) + c.extra_s;
  c.energy_j = k.flops * pj_per_flop(k.prec) * 1e-12 + moved * 8 * p.get("pj_per_bit_internal") * 1e-12 +
               k.halo_bytes * 8 * 15e-12;
  return c;
}
double NMPDevice::tdp_w() const {
  double f = p.get("unit_freq_mhz") / 350.0;
  return 10 + p.get("units") * f * (0.1 + 0.002 * (p.get("fp_ops_per_cycle_per_unit") + p.get("int_ops_per_cycle_per_unit"))) +
         p.get("units") * p.get("unit_bw_gbps") * 0.005;
}
double NMPDevice::cost_usd() const {
  double f = p.get("unit_freq_mhz") / 350.0;
  return 200 + p.get("units") * 3.5 * f *
                   (1 + 0.1 * (p.get("fp_ops_per_cycle_per_unit") + p.get("int_ops_per_cycle_per_unit")) +
                    p.get("unit_bw_gbps") / 8);
}

// ---------------------------------------------------------------------------
// CSD 可计算存储
// ---------------------------------------------------------------------------
CSDDevice::CSDDevice(const std::string& id_) : ComputeDevice(id_, "CSD") {
  // 默认模板: SmartSSD 量级（内嵌 FPGA，≈0.3 TFLOPS FP32，4GB DRAM 缓冲）
  p.add("compute_gflops_fp32", 300, 1, 20000, "GFLOPS", "盘内处理器 FP32 算力");
  p.add("int8_ratio", 4, 1, 8, "×", "INT8 相对 FP32 吞吐");
  p.add("dram_buffer_gb", 4, 0.25, 64, "GB", "盘内 DRAM 缓冲容量");
  p.add("buffer_bw_gbps", 19, 1, 200, "GB/s", "盘内 DRAM 缓冲带宽");
  p.add("outstanding", 256, 16, 4096, "个", "盘内可并发 NAND 请求");
  add_common(50, 3, 4, 0, 5);
}
double CSDDevice::peak_flops(Precision prec, OpClass) const {
  double f = p.get("compute_gflops_fp32") * 1e9;
  switch (prec) {
    case Precision::FP32: return f;
    case Precision::FP16:
    case Precision::BF16: return f * 2;
    case Precision::INT8:
    case Precision::FP8: return f * p.get("int8_ratio");
    case Precision::INT4: return f * p.get("int8_ratio") * 2;
  }
  return f;
}
bool CSDDevice::supports(const Kernel&, std::string*) const { return true; }
double CSDDevice::op_efficiency(OpClass op) const {
  switch (op) {
    case OpClass::GEMM: return 0.6;
    case OpClass::SCAN: return 0.9;
    default: return 0.7;
  }
}
double CSDDevice::tdp_w() const { return 10 + p.get("compute_gflops_fp32") * 0.03 + p.get("dram_buffer_gb") * 0.4; }
double CSDDevice::cost_usd() const { return 300 + p.get("compute_gflops_fp32") * 1.5 + p.get("dram_buffer_gb") * 5; }

// ---------------------------------------------------------------------------
// 存算一体 RRAM / MRAM
// ---------------------------------------------------------------------------
CIMDevice::CIMDevice(const std::string& id_, bool mram_) : ComputeDevice(id_, mram_ ? "CIM_MRAM" : "CIM_RRAM"), mram(mram_) {
  p.add("xbar_rows", 256, 64, 1024, "", "交叉阵列行数", true, {64, 128, 256, 512, 1024});
  p.add("xbar_cols", 256, 64, 1024, "", "交叉阵列列数", true, {64, 128, 256, 512, 1024});
  p.add("num_xbars", 4096, 1, 65536, "个", "交叉阵列数", true);
  p.add("rows_active", 32, 1, 1024, "", "每次同时激活的行数（决定 ADC 所需位数）", true);
  if (!mram) {
    p.add("cell_bits", 2, 1, 4, "bit", "每单元存储位数（RRAM 多值，≤4）", true);
    p.add("adc_bits", 7, 1, 10, "bit", "ADC 精度", true);
    p.add("write_latency_ns", 1000, 50, 100000, "ns", "单行写入时间（含 write-verify）");
    p.add("write_energy_pj_per_cell", 10, 0.1, 1000, "pJ", "单元写入能耗");
    p.add("endurance", 1e7, 1e4, 1e10, "次", "单元写入耐久");
  } else {
    p.add("cell_bits", 1, 1, 2, "bit", "每单元存储位数（MRAM 隧穿磁阻比低，≤2）", true);
    p.add("adc_bits", 6, 1, 10, "bit", "ADC 精度", true);
    p.add("write_latency_ns", 20, 2, 1000, "ns", "单行写入时间");
    p.add("write_energy_pj_per_cell", 1, 0.01, 100, "pJ", "单元写入能耗");
    p.add("endurance", 1e12, 1e8, 1e16, "次", "单元写入耐久");
  }
  p.add("adcs_per_xbar", 32, 1, 1024, "个", "每阵列 ADC 数（列复用）", true);
  p.add("adc_rate_msps", 500, 10, 5000, "MS/s", "ADC 采样率");
  p.add("adc_energy_pj", 0.1, 0.01, 100, "pJ", "单次 ADC 转换能耗");
  p.add("dac_bits", 1, 1, 8, "bit", "每周期输入位数（位串行=1）", true);
  p.add("parallel_write_xbars", 64, 1, 65536, "个", "可并行写入的阵列数（写驱动/电荷泵限制）", true);
  p.add("digital_gops", 500, 1, 50000, "GOPS", "数字外围（累加/激活/比较）算力");
  p.add("buffer_mb", 32, 0.5, 256, "MB", "片上 SRAM 缓冲");
  add_common(10, 4, 16, 0, 1.0);
}
void CIMDevice::validate(std::vector<std::string>& errors) const {
  ComputeDevice::validate(errors);
  const std::string ctx = "设备 '" + id + "'";
  if (p.get("adcs_per_xbar") > p.get("xbar_cols")) errors.push_back(ctx + ": 每阵列 ADC 数不能超过列数");
  if (p.get("rows_active") > p.get("xbar_rows")) errors.push_back(ctx + ": 激活行数不能超过阵列行数");
}
int CIMDevice::slices(int wbits) const { return static_cast<int>(std::ceil(wbits / p.get("cell_bits"))); }
double CIMDevice::t_mvm(int input_bits) const {
  double passes = std::ceil(input_bits / p.get("dac_bits"));
  double groups = std::ceil(p.get("xbar_rows") / p.get("rows_active"));
  double colc = std::ceil(p.get("xbar_cols") / p.get("adcs_per_xbar"));
  return passes * groups * colc / (p.get("adc_rate_msps") * 1e6);
}
double CIMDevice::peak_flops(Precision prec, OpClass op) const {
  if (op == OpClass::GEMM) {
    if (prec != Precision::INT8 && prec != Precision::INT4) return 0;
    int b = precision_bits(prec);
    double macs = p.get("num_xbars") * p.get("xbar_rows") * p.get("xbar_cols") / slices(b);
    return 2 * macs / t_mvm(b);
  }
  if (is_float(prec) && prec != Precision::FP16 && prec != Precision::FP32) return 0;
  return p.get("digital_gops") * 1e9;
}
bool CIMDevice::supports(const Kernel& k, std::string* why) const {
  if (k.op == OpClass::SPMM || k.op == OpClass::SORT || k.op == OpClass::SCATTER) {
    if (why) *why = std::string(type) + " 不支持不规则算子 " + op_name(k.op) + "（交叉阵列只适合稠密矩阵-向量乘）";
    return false;
  }
  if (k.op == OpClass::GEMM && k.prec != Precision::INT8 && k.prec != Precision::INT4) {
    if (why) *why = std::string(type) + " 的模拟交叉阵列只支持 int8/int4 定点 GEMM（当前精度 " + precision_name(k.prec) + "）";
    return false;
  }
  return true;
}
KernelCost CIMDevice::estimate(const Kernel& k, bool commit) {
  KernelCost c;
  std::string why;
  if (!supports(k, &why)) {
    c.supported = false;
    c.reason = why;
    return c;
  }
  double bw = mem ? mem->seq_bw() : 50e9;
  c.mem_s = k.seq_bytes / bw;
  c.setup_s = launch_s();
  double mem_pj = mem ? mem->pj_per_bit() : 10;
  if (k.op != OpClass::GEMM) {
    c.peak_flops = p.get("digital_gops") * 1e9;
    c.compute_s = k.flops / c.peak_flops + k.rand_accesses * 2e-9 / std::max(1.0, p.get("num_xbars") / 64);
    c.time_s = c.setup_s + std::max(c.compute_s, c.mem_s);
    c.energy_j = k.flops * 0.5e-12 + k.seq_bytes * 8 * mem_pj * 1e-12;
    return c;
  }
  int b = precision_bits(k.prec);
  double K = std::max(1.0, k.gemm_k), N = std::max(1.0, k.gemm_n);
  double M = k.gemm_m > 0 ? k.gemm_m : std::max(1.0, k.flops / (2 * K * N));
  double rows = p.get("xbar_rows"), cols = p.get("xbar_cols"), X = p.get("num_xbars");
  int s = slices(b);
  double rt = std::ceil(K / rows), ct = std::ceil(N / cols);
  double tiles = rt * ct * s;
  double tm = t_mvm(b);
  double prog_one = rows * p.get("write_latency_ns") * NS;
  double par = p.get("parallel_write_xbars");
  double program_s = 0, cell_writes = 0, compute_s;
  double inpasses = std::ceil(b / p.get("dac_bits")) * std::ceil(rows / p.get("rows_active"));
  if (tiles <= X) {
    bool need = !k.weights_static || k.weight_id.empty() || !programmed_.count(k.weight_id);
    if (need) {
      program_s = std::ceil(tiles / par) * prog_one;
      cell_writes = tiles * rows * cols;
      if (commit) {
        if (!k.weight_id.empty()) programmed_.insert(k.weight_id);
        max_cell_writes_ += 1;
      }
    }
    double repl = std::floor(X / tiles);
    compute_s = std::ceil(M / repl) * tm;
  } else {
    double passes = std::ceil(tiles / X);
    program_s = passes * std::ceil(X / par) * prog_one;
    cell_writes = passes * X * rows * cols;
    compute_s = passes * M * tm;
    if (commit) {
      programmed_.clear();
      max_cell_writes_ += passes;
    }
    c.warnings.push_back(id + ": 权重 " + ParamSet::fmt(tiles) + " 个阵列块超过容量 " + ParamSet::fmt(X) +
                         "，每次调用需分 " + ParamSet::fmt(passes) + " 轮重新编程");
  }
  double digital_s = M * N * rt * s / (p.get("digital_gops") * 1e9);
  c.compute_s = compute_s + digital_s;
  c.extra_s = program_s;
  c.peak_flops = peak_flops(k.prec, k.op);
  c.time_s = c.setup_s + program_s + std::max(c.compute_s, c.mem_s);
  double conv = M * tiles * inpasses * cols;
  c.energy_j = conv * p.get("adc_energy_pj") * 1e-12 + cell_writes * p.get("write_energy_pj_per_cell") * 1e-12 +
               k.seq_bytes * 8 * mem_pj * 1e-12;
  double req = std::ceil(std::log2(p.get("rows_active"))) + p.get("cell_bits") + p.get("dac_bits") - 1;
  if (p.get("adc_bits") < req)
    c.warnings.push_back(id + ": ADC 精度 " + ParamSet::fmt(p.get("adc_bits")) + " bit 低于无损所需 " +
                         ParamSet::fmt(req) + " bit，部分和被截断（结果存在精度损失）");
  if (commit && max_cell_writes_ > 0.01 * p.get("endurance"))
    c.warnings.push_back(id + ": 单元写入次数已达耐久上限的 " + ParamSet::fmt(100 * endurance_used()) + "%");
  return c;
}
double CIMDevice::tdp_w() const {
  double adc = p.get("num_xbars") * p.get("adcs_per_xbar") * p.get("adc_rate_msps") * 1e6 * p.get("adc_energy_pj") * 1e-12;
  return 5 + adc + p.get("digital_gops") * 0.005 + p.get("buffer_mb") * 0.05;
}
double CIMDevice::cost_usd() const {
  double cells = p.get("num_xbars") * p.get("xbar_rows") * p.get("xbar_cols");
  return 200 + cells * p.get("cell_bits") / 1e6 * (mram ? 4.0 : 2.0) +
         p.get("num_xbars") * p.get("adcs_per_xbar") * 0.01 * std::pow(2, p.get("adc_bits") - 6) +
         p.get("digital_gops") * 0.2 + p.get("buffer_mb") * 4;
}
json CIMDevice::describe_derived() const {
  json o = ComputeDevice::describe_derived();
  o["t_mvm_int8_ns"] = t_mvm(8) / NS;
  o["weight_capacity_int8_MB"] = p.get("num_xbars") * p.get("xbar_rows") * p.get("xbar_cols") / slices(8) / MB;
  o["full_reprogram_ms"] = std::ceil(p.get("num_xbars") / p.get("parallel_write_xbars")) * p.get("xbar_rows") *
                           p.get("write_latency_ns") * NS / MS;
  o["endurance"] = p.get("endurance");
  return o;
}

// ---------------------------------------------------------------------------
// 纯存储设备
// ---------------------------------------------------------------------------
StorageDevice::StorageDevice(const std::string& id_, const std::string& type_) : ComputeDevice(id_, type_) {
  if (type == "MEM_EXPANDER") {
    p.add("controller_latency_ns", 70, 20, 1000, "ns", "扩展器控制器附加延迟");
    add_common(0.1, 5, 16, 2.0, 0);
  } else {
    add_common(0.1, 4, 4, 0, 0);
  }
}
std::vector<std::string> StorageDevice::allowed_memory_kinds() const {
  if (type == "SSD") return {"FLASH"};
  return {"DRAM", "SCM"};
}
void StorageDevice::validate(std::vector<std::string>&) const {}
double StorageDevice::tdp_w() const { return type == "SSD" ? 3 : 8; }
double StorageDevice::cost_usd() const { return type == "SSD" ? 30 : 300; }

// ---------------------------------------------------------------------------
std::vector<std::string> device_types() {
  return {"CPU", "GPU", "NPU", "TPU", "FPGA", "NMP", "CSD", "CIM_RRAM", "CIM_MRAM", "SSD", "MEM_EXPANDER"};
}

std::unique_ptr<ComputeDevice> make_device(const std::string& id, const std::string& type, std::string* err) {
  if (type == "CPU") return std::make_unique<CPUDevice>(id);
  if (type == "GPU") return std::make_unique<GPUDevice>(id);
  if (type == "NPU") return std::make_unique<NPUDevice>(id);
  if (type == "TPU") return std::make_unique<TPUDevice>(id);
  if (type == "FPGA") return std::make_unique<FPGADevice>(id);
  if (type == "NMP") return std::make_unique<NMPDevice>(id);
  if (type == "CSD") return std::make_unique<CSDDevice>(id);
  if (type == "CIM_RRAM") return std::make_unique<CIMDevice>(id, false);
  if (type == "CIM_MRAM") return std::make_unique<CIMDevice>(id, true);
  if (type == "SSD" || type == "MEM_EXPANDER") return std::make_unique<StorageDevice>(id, type);
  if (err) *err = "未知设备类型 '" + type + "'";
  return nullptr;
}

}  // namespace hgc
