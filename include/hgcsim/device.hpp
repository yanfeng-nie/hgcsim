// =============================================================================
// device.hpp : 计算设备抽象
//
// 设备类型：
//   CPU            主机通用处理器（SIMD/AMX）
//   GPU            图形处理器（SM + Tensor Core）
//   NPU            神经网络处理器（Cube 矩阵单元 + 向量单元，参考昇腾类架构）
//   TPU            张量处理器（脉动阵列 MXU + 向量单元 + SparseCore）
//   FPGA           可重构加速卡（DSP/LUT，定制流水线，存在重配置开销）
//   NMP            近内存计算（PIM，参考 UPMEM / HBM-PIM / CXL-PNM）
//   CSD            可计算存储（SSD 内嵌 FPGA/ARM，享有盘内高带宽）
//   CIM_RRAM       RRAM 存算一体（模拟交叉阵列，写慢、耐久有限）
//   CIM_MRAM       MRAM 存算一体（写快、耐久高、单元位数低）
//   SSD            纯存储设备（无计算）
//   MEM_EXPANDER   CXL Type-3 内存扩展器 / DIMM 形态 SCM（无计算）
//
// 性能模型（扩展 Roofline）：
//   t = 启动开销 + max(t_compute, t_mem) + 重叠惩罚 × min(t_compute, t_mem) + 设备特有开销
//   t_compute = FLOPs / (峰值(精度, 算子) × 算子效率 × 形状效率)
//   t_mem     = 顺序字节/顺序带宽 + 随机访问时间（考虑缓存命中率、访问粒度放大、Little 定律并发上限）
// 峰值一律由结构参数推导（单元数 × 频率 × 每周期操作数），不可直接填写。
// =============================================================================
#pragma once
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "hgcsim/memory.hpp"
#include "hgcsim/params.hpp"

namespace hgc {

enum class Precision { FP32, FP16, BF16, FP8, INT8, INT4 };
enum class OpClass { GEMM, SPMM, SORT, SCATTER, ELEMWISE, REDUCE, TOPK, SCAN };

const char* precision_name(Precision p);
bool parse_precision(const std::string& s, Precision& out);
double precision_bytes(Precision p);
int precision_bits(Precision p);
const char* op_name(OpClass o);

// 一个计算任务（kernel）的抽象工作量描述 —— 由负载生成器从数据集参数解析推导
struct Kernel {
  std::string name;
  std::string stage;
  OpClass op = OpClass::ELEMWISE;
  Precision prec = Precision::FP32;
  double flops = 0;            // 有效运算量
  double seq_bytes = 0;        // 顺序流式访问字节（读+写）
  double rand_accesses = 0;    // 随机访问次数
  double rand_elem_bytes = 0;  // 每次随机访问的有用字节
  double working_set = 0;      // 随机访问所覆盖的数据集大小（决定缓存命中率）
  double skew = 0;             // 访问偏斜度 0~1（度分布幂律，越大热点越集中）
  double gemm_m = 0, gemm_n = 0, gemm_k = 0;  // GEMM 形状 C[m×n] = A[m×k]·W[k×n]
  std::string weight_id;       // 权重张量标识（存算一体/FPGA 判断是否需要重新编程）
  bool weights_static = true;  // 权重在多次调用间是否保持不变
  double halo_bytes = 0;       // 跨分区交换字节（NMP 单元间通信使用）
};

struct KernelCost {
  bool supported = true;
  std::string reason;
  double time_s = 0;
  double compute_s = 0;
  double mem_s = 0;
  double setup_s = 0;
  double extra_s = 0;  // 设备特有开销：重配置 / 权重编程 / 单元间通信
  double energy_j = 0;
  double peak_flops = 0;
  std::vector<std::string> warnings;
};

class ComputeDevice {
 public:
  ComputeDevice(std::string id, std::string type);
  virtual ~ComputeDevice() = default;

  std::string id;
  std::string type;
  ParamSet p;
  MemoryTier* mem = nullptr;   // 本地存储（CPU 为主机 DRAM）
  double attach_bw = 0;        // 与上游连接链路的有效带宽（系统构建时填入）

  virtual bool has_compute() const { return true; }
  virtual bool needs_local_memory() const { return true; }
  virtual std::vector<std::string> allowed_memory_kinds() const { return {"HBM", "GDDR", "DRAM"}; }
  virtual void validate(std::vector<std::string>& errors) const;

  // ---- 性能模型接口 ----
  virtual double peak_flops(Precision prec, OpClass op) const;
  virtual bool supports(const Kernel& k, std::string* why) const;
  virtual double op_efficiency(OpClass op) const;
  virtual double shape_efficiency(const Kernel& k) const;
  virtual double cache_bytes() const { return 0; }
  virtual double cache_bw() const { return 1e12; }
  virtual double outstanding() const { return 64; }
  virtual double random_boost() const { return 1.0; }  // 自定义访存引擎对随机访问的加成
  virtual double launch_s() const { return p.get("launch_overhead_us") * US; }
  virtual double overlap_penalty() const { return 0.1; }
  virtual double pj_per_flop(Precision prec) const;

  // commit=true 时会更新设备内部状态（例如存算一体已编程权重、FPGA 已加载比特流）
  virtual KernelCost estimate(const Kernel& k, bool commit);

  // ---- 预算模型 ----
  virtual double tdp_w() const { return 0; }
  virtual double idle_w() const { return tdp_w() * 0.15; }
  virtual double cost_usd() const { return 0; }

  virtual json describe_derived() const;
  virtual void reset_state() {}

  // 设备侧接口能力
  int max_link_gen() const { return p.geti("max_link_gen"); }
  int max_lanes() const { return p.geti("max_lanes"); }
  double cxl_max_version() const { return p.get("cxl_max_version"); }

  // 运行时统计
  double busy_s = 0, energy_j = 0;
  long kernels = 0;
  std::vector<std::string> warnings;
  void reset_stats() {
    busy_s = energy_j = 0;
    kernels = 0;
    warnings.clear();
    reset_state();
  }

 protected:
  KernelCost roofline(const Kernel& k, double peak) const;
  void add_common(double launch_us, double link_gen, double lanes, double cxl_ver, double pj_flop);
};

// ---------------- 具体设备 ----------------
class CPUDevice : public ComputeDevice {
 public:
  explicit CPUDevice(const std::string& id);
  std::vector<std::string> allowed_memory_kinds() const override { return {"DRAM"}; }
  double peak_flops(Precision prec, OpClass op) const override;
  bool supports(const Kernel& k, std::string* why) const override;
  double op_efficiency(OpClass op) const override;
  double cache_bytes() const override { return p.get("llc_mb") * MB; }
  double cache_bw() const override;
  double outstanding() const override { return p.get("cores") * p.get("lfb_per_core"); }
  double tdp_w() const override;
  double cost_usd() const override;
};

class GPUDevice : public ComputeDevice {
 public:
  explicit GPUDevice(const std::string& id);
  double peak_flops(Precision prec, OpClass op) const override;
  bool supports(const Kernel& k, std::string* why) const override;
  double op_efficiency(OpClass op) const override;
  double shape_efficiency(const Kernel& k) const override;
  double cache_bytes() const override { return p.get("l2_mb") * MB; }
  double cache_bw() const override;
  double outstanding() const override { return p.get("sms") * p.get("outstanding_per_sm"); }
  double tdp_w() const override;
  double cost_usd() const override;
};

class NPUDevice : public ComputeDevice {
 public:
  explicit NPUDevice(const std::string& id);
  double peak_flops(Precision prec, OpClass op) const override;
  bool supports(const Kernel& k, std::string* why) const override;
  double op_efficiency(OpClass op) const override;
  double shape_efficiency(const Kernel& k) const override;
  double cache_bytes() const override { return p.get("buffer_mb") * MB; }
  double cache_bw() const override;
  double outstanding() const override { return p.get("ai_cores") * p.get("outstanding_per_core"); }
  double random_boost() const override { return p.get("gather_efficiency"); }
  double tdp_w() const override;
  double cost_usd() const override;
};

class TPUDevice : public ComputeDevice {
 public:
  explicit TPUDevice(const std::string& id);
  double peak_flops(Precision prec, OpClass op) const override;
  bool supports(const Kernel& k, std::string* why) const override;
  double op_efficiency(OpClass op) const override;
  double shape_efficiency(const Kernel& k) const override;
  double cache_bytes() const override { return p.get("vmem_mb") * MB; }
  double cache_bw() const override;
  double outstanding() const override { return p.get("outstanding") + p.get("sparsecores") * 2048; }
  double random_boost() const override;
  double tdp_w() const override;
  double cost_usd() const override;
};

class FPGADevice : public ComputeDevice {
 public:
  explicit FPGADevice(const std::string& id);
  void validate(std::vector<std::string>& errors) const override;
  double peak_flops(Precision prec, OpClass op) const override;
  bool supports(const Kernel& k, std::string* why) const override;
  double op_efficiency(OpClass op) const override;
  double cache_bytes() const override { return p.get("onchip_mb") * MB; }
  double cache_bw() const override;
  double outstanding() const override { return p.get("outstanding"); }
  double random_boost() const override { return 1.5; }
  double overlap_penalty() const override { return 0.0; }  // 数据流架构，访存与计算完全重叠
  KernelCost estimate(const Kernel& k, bool commit) override;
  double tdp_w() const override;
  double cost_usd() const override;
  void reset_state() override { loaded_.clear(); }

 private:
  std::deque<OpClass> loaded_;  // 已加载的内核（LRU）
};

class NMPDevice : public ComputeDevice {
 public:
  explicit NMPDevice(const std::string& id);
  std::vector<std::string> allowed_memory_kinds() const override { return {"DRAM", "HBM"}; }
  void validate(std::vector<std::string>& errors) const override;
  double peak_flops(Precision prec, OpClass op) const override;
  bool supports(const Kernel& k, std::string* why) const override;
  double op_efficiency(OpClass op) const override;
  KernelCost estimate(const Kernel& k, bool commit) override;
  double internal_bw() const { return p.get("units") * p.get("unit_bw_gbps") * GBps; }
  double tdp_w() const override;
  double cost_usd() const override;
};

class CSDDevice : public ComputeDevice {
 public:
  explicit CSDDevice(const std::string& id);
  std::vector<std::string> allowed_memory_kinds() const override { return {"FLASH"}; }
  double peak_flops(Precision prec, OpClass op) const override;
  bool supports(const Kernel& k, std::string* why) const override;
  double op_efficiency(OpClass op) const override;
  double cache_bytes() const override { return p.get("dram_buffer_gb") * GB; }
  double cache_bw() const override { return p.get("buffer_bw_gbps") * GBps; }
  double outstanding() const override { return p.get("outstanding"); }
  double tdp_w() const override;
  double cost_usd() const override;
};

class CIMDevice : public ComputeDevice {
 public:
  CIMDevice(const std::string& id, bool mram);
  bool mram;
  void validate(std::vector<std::string>& errors) const override;
  double peak_flops(Precision prec, OpClass op) const override;
  bool supports(const Kernel& k, std::string* why) const override;
  double op_efficiency(OpClass op) const override { return 1.0; }
  KernelCost estimate(const Kernel& k, bool commit) override;
  double cache_bytes() const override { return p.get("buffer_mb") * MB; }
  double tdp_w() const override;
  double cost_usd() const override;
  json describe_derived() const override;
  void reset_state() override {
    programmed_.clear();
    max_cell_writes_ = 0;
  }
  double t_mvm(int input_bits) const;
  int slices(int weight_bits) const;
  double endurance_used() const { return max_cell_writes_ / p.get("endurance"); }

 private:
  std::set<std::string> programmed_;
  double max_cell_writes_ = 0;
};

// 纯存储类设备（无计算能力）
class StorageDevice : public ComputeDevice {
 public:
  StorageDevice(const std::string& id, const std::string& type);
  bool has_compute() const override { return false; }
  std::vector<std::string> allowed_memory_kinds() const override;
  void validate(std::vector<std::string>& errors) const override;
  double peak_flops(Precision, OpClass) const override { return 0; }
  bool supports(const Kernel&, std::string* why) const override {
    if (why) *why = type + " 为纯存储设备，没有计算能力";
    return false;
  }
  double controller_latency_s() const { return p.has("controller_latency_ns") ? p.get("controller_latency_ns") * NS : 0; }
  double tdp_w() const override;
  double cost_usd() const override;
};

std::unique_ptr<ComputeDevice> make_device(const std::string& id, const std::string& type, std::string* err);
std::vector<std::string> device_types();

}  // namespace hgc
