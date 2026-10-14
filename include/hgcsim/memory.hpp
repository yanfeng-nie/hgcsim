// =============================================================================
// memory.hpp : 存储层次抽象 —— HBM / GDDR / DRAM / SCM / Flash
//
// 每个存储层级由一组“结构参数”描述，峰值带宽等性能指标由结构参数推导，
// 而不是直接填写（保证参数之间关系自洽）：
//   HBM  : 峰值带宽 = 堆栈数 × 接口位宽 × 每引脚速率 / 8
//   GDDR : 峰值带宽 = 总线位宽 × 每引脚速率 / 8
//   DRAM : 峰值带宽 = 通道数 × 通道位宽 × 数据速率(MT/s) / 8
//   SCM  : 带宽 = 模块数 × 单模块带宽
//   Flash: 内部带宽 = min(NAND通道数×通道速率, die数×页大小/tR, 主控上限)
//          随机 IOPS = min(主控 IOPS 上限, die数 / tR)
// =============================================================================
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "hgcsim/params.hpp"

namespace hgc {

enum class MemKind { HBM, GDDR, DRAM, SCM, FLASH };

const char* mem_kind_name(MemKind k);
bool parse_mem_kind(const std::string& s, MemKind& out);

class MemoryTier {
 public:
  MemoryTier(std::string id, MemKind kind);

  std::string id;     // 全局唯一，例如 "dram0"、"gpu0.mem"、"ssd0.flash"
  std::string owner;  // 所属设备 id（主机内存为 "cpu0"）
  MemKind kind;
  ParamSet p;

  // ---- 关系约束校验 ----
  void validate(std::vector<std::string>& errors) const;

  // ---- 推导出的性能指标 ----
  double capacity_bytes() const;
  double peak_bw() const;        // 理论峰值带宽 (bytes/s)
  double seq_read_bw() const;    // 可达顺序读带宽
  double seq_write_bw() const;   // 可达顺序写带宽
  double seq_bw() const { return seq_read_bw(); }
  double latency_s() const;      // 访问延迟（加载到使用）
  double granularity() const;    // 最小访问粒度 (bytes)
  double pj_per_bit() const;
  bool byte_addressable() const { return kind != MemKind::FLASH; }

  // 随机访问时的“有效”带宽（只计有用字节）
  //   elem_bytes : 每次随机访问需要的有用字节数
  //   outstanding: 发起方可维持的并发访存请求数（Little 定律：吞吐 ≤ 并发/延迟）
  double random_useful_bw(double elem_bytes, double outstanding) const;
  // 读放大系数：访问粒度对齐带来的额外搬运
  double amplification(double elem_bytes) const;

  // ---- 成本 / 功耗（用于预算约束，简化估算模型）----
  double cost_usd() const;
  double static_power_w() const;
  double peak_power_w() const;

  json describe_derived() const;

  // ---- 运行时统计 ----
  double used = 0, peak_used = 0;
  double bytes_read = 0, bytes_written = 0;
  void reset_stats() { used = peak_used = bytes_read = bytes_written = 0; }
};

std::unique_ptr<MemoryTier> make_memory(const std::string& id, MemKind kind);

}  // namespace hgc
