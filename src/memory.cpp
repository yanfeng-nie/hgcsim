#include "hgcsim/memory.hpp"

#include <algorithm>
#include <cmath>

namespace hgc {

const char* mem_kind_name(MemKind k) {
  switch (k) {
    case MemKind::HBM: return "HBM";
    case MemKind::GDDR: return "GDDR";
    case MemKind::DRAM: return "DRAM";
    case MemKind::SCM: return "SCM";
    case MemKind::FLASH: return "FLASH";
  }
  return "?";
}

bool parse_mem_kind(const std::string& s, MemKind& out) {
  if (s == "HBM") out = MemKind::HBM;
  else if (s == "GDDR") out = MemKind::GDDR;
  else if (s == "DRAM") out = MemKind::DRAM;
  else if (s == "SCM") out = MemKind::SCM;
  else if (s == "FLASH") out = MemKind::FLASH;
  else return false;
  return true;
}

MemoryTier::MemoryTier(std::string id_, MemKind k) : id(std::move(id_)), kind(k) {
  switch (kind) {
    case MemKind::HBM:
      // 默认: HBM3 4 堆栈 (≈1.6 TB/s, 64 GB)
      p.add("stacks", 4, 1, 16, "个", "HBM 堆栈数", true);
      p.add("interface_bits", 1024, 1024, 2048, "bit", "每堆栈接口位宽 (HBM3/3E=1024, HBM4=2048)", true,
            {1024, 2048});
      p.add("gbps_per_pin", 6.4, 1.0, 12.0, "Gb/s", "每引脚数据速率 (HBM3 6.4, HBM3E 9.6, HBM4 8~11)");
      p.add("gb_per_stack", 16, 1, 64, "GB", "单堆栈容量 (HBM3E 12-Hi 36GB, HBM4 16-Hi 64GB)");
      p.add("efficiency", 0.85, 0.3, 0.98, "", "顺序访问可达带宽比例");
      p.add("random_efficiency", 0.45, 0.05, 0.9, "", "随机访问时带宽利用率（bank 冲突等）");
      p.add("latency_ns", 300, 60, 800, "ns", "从所属设备看到的加载到使用延迟");
      p.add("granularity_bytes", 32, 32, 64, "B", "最小访问粒度 (sector)", true, {32, 64});
      p.add("pj_per_bit", 3.9, 1.0, 10.0, "pJ/bit", "访问能耗");
      break;
    case MemKind::GDDR:
      // 默认: GDDR6X 192bit 21Gbps 12GB (≈504 GB/s, 消费级显卡)
      p.add("bus_width_bits", 192, 32, 512, "bit", "显存总线位宽", true,
            {32, 64, 96, 128, 160, 192, 256, 320, 384, 448, 512});
      p.add("gbps_per_pin", 21, 8, 36, "Gb/s", "每引脚速率 (GDDR6 16~20, GDDR6X 21~24, GDDR7 28~36)");
      p.add("capacity_gb", 12, 1, 96, "GB", "显存容量");
      p.add("efficiency", 0.85, 0.3, 0.98, "", "顺序访问可达带宽比例");
      p.add("random_efficiency", 0.40, 0.05, 0.9, "", "随机访问带宽利用率");
      p.add("latency_ns", 350, 150, 900, "ns", "加载到使用延迟");
      p.add("granularity_bytes", 32, 32, 64, "B", "最小访问粒度", true, {32, 64});
      p.add("pj_per_bit", 7.0, 3.0, 15.0, "pJ/bit", "访问能耗");
      break;
    case MemKind::DRAM:
      // 默认: DDR5-5600 双通道 32GB (≈89.6 GB/s 峰值)
      p.add("channels", 2, 1, 24, "个", "内存通道数 (双路 12 通道服务器=24)", true);
      p.add("data_rate_mtps", 5600, 1600, 8800, "MT/s", "数据速率 (DDR5 最高 6400, MRDIMM 8800)");
      p.add("channel_width_bits", 64, 16, 64, "bit", "通道位宽 (DDR=64, LPDDR=16/32)", true, {16, 32, 64});
      p.add("capacity_gb", 32, 1, 12288, "GB", "容量");
      p.add("efficiency", 0.75, 0.3, 0.95, "", "顺序访问可达带宽比例");
      p.add("random_efficiency", 0.35, 0.05, 0.9, "", "随机访问带宽利用率");
      p.add("latency_ns", 90, 50, 250, "ns", "加载到使用延迟");
      p.add("granularity_bytes", 64, 32, 64, "B", "缓存行粒度", true, {32, 64});
      p.add("pj_per_bit", 15, 3, 40, "pJ/bit", "访问能耗（含 I/O）");
      break;
    case MemKind::SCM:
      // 默认: 4 条 Optane 类持久内存模块（参考已停产的 Optane PMem 200）
      p.add("modules", 4, 1, 32, "个", "SCM 模块数", true);
      p.add("read_gbps_per_module", 6.6, 0.5, 25, "GB/s", "单模块顺序读带宽");
      p.add("write_gbps_per_module", 2.3, 0.2, 15, "GB/s", "单模块顺序写带宽");
      p.add("gb_per_module", 256, 16, 1024, "GB", "单模块容量");
      p.add("read_latency_ns", 350, 100, 5000, "ns", "读延迟");
      p.add("write_latency_ns", 500, 100, 10000, "ns", "写延迟");
      p.add("random_efficiency", 0.5, 0.05, 0.95, "", "随机访问带宽利用率");
      p.add("granularity_bytes", 256, 64, 4096, "B", "介质访问粒度", true, {64, 128, 256, 512, 4096});
      p.add("pj_per_bit", 40, 5, 150, "pJ/bit", "访问能耗");
      break;
    case MemKind::FLASH:
      // 默认: 消费级 PCIe 4.0 NVMe 2TB
      p.add("nand_channels", 8, 1, 32, "个", "NAND 通道数", true);
      p.add("channel_mtps", 2400, 200, 4800, "MT/s", "NAND 接口速率 (ONFI 5.x 3600)");
      p.add("dies", 64, 1, 1024, "个", "NAND die 总数（并行度）", true);
      p.add("page_kb", 16, 4, 32, "KB", "NAND 页大小", true, {4, 8, 16, 32});
      p.add("read_latency_us", 50, 3, 200, "us", "页读延迟 tR (TLC≈50, SLC/XL-Flash≈3~5)");
      p.add("program_latency_us", 600, 20, 3000, "us", "页编程延迟 tProg");
      p.add("capacity_tb", 2, 0.064, 256, "TB", "容量");
      p.add("controller_iops_m", 1.2, 0.05, 10, "M IOPS", "主控 4KB 随机读 IOPS 上限");
      p.add("controller_bw_gbps", 14, 0.5, 64, "GB/s", "主控后端带宽上限");
      p.add("pj_per_bit", 100, 10, 1000, "pJ/bit", "访问能耗");
      break;
  }
}

void MemoryTier::validate(std::vector<std::string>& errors) const {
  const std::string ctx = "存储 '" + id + "'";
  switch (kind) {
    case MemKind::HBM: {
      int ib = p.geti("interface_bits");
      if (ib == 1024 && p.get("gbps_per_pin") > 9.8)
        errors.push_back(ctx + ": HBM3/3E (1024bit) 每引脚速率上限为 9.8 Gb/s，更高速率需 HBM4 (2048bit)");
      if (ib == 1024 && p.get("gb_per_stack") > 48)
        errors.push_back(ctx + ": HBM3E 单堆栈容量上限 48 GB (16-Hi)，64 GB 需 HBM4");
      break;
    }
    case MemKind::GDDR: {
      double chips = p.get("bus_width_bits") / 32.0;
      if (p.get("capacity_gb") > chips * 6.0 + 1e-9)
        errors.push_back(ctx + ": GDDR 容量超过 " + ParamSet::fmt(chips * 6) +
                         " GB (每 32bit 颗粒最大 3GB, 双面贴装×2)");
      break;
    }
    case MemKind::DRAM: {
      double maxcap = p.get("channels") * 512.0;  // 每通道 2 DIMM × 256GB
      if (p.get("capacity_gb") > maxcap + 1e-9)
        errors.push_back(ctx + ": 容量超过 " + ParamSet::fmt(maxcap) + " GB (每通道最多 2×256GB DIMM)");
      break;
    }
    case MemKind::SCM:
      if (p.get("write_gbps_per_module") > p.get("read_gbps_per_module"))
        errors.push_back(ctx + ": SCM 写带宽不应高于读带宽");
      break;
    case MemKind::FLASH:
      if (p.get("capacity_tb") > p.get("dies") * 0.256 + 1e-9)
        errors.push_back(ctx + ": 容量超过 die 数 × 2Tb(256GB) = " + ParamSet::fmt(p.get("dies") * 0.256) +
                         " TB");
      if (p.get("program_latency_us") < p.get("read_latency_us"))
        errors.push_back(ctx + ": 编程延迟 tProg 不应小于读延迟 tR");
      break;
  }
}

double MemoryTier::capacity_bytes() const {
  switch (kind) {
    case MemKind::HBM: return p.get("stacks") * p.get("gb_per_stack") * GB;
    case MemKind::GDDR: return p.get("capacity_gb") * GB;
    case MemKind::DRAM: return p.get("capacity_gb") * GB;
    case MemKind::SCM: return p.get("modules") * p.get("gb_per_module") * GB;
    case MemKind::FLASH: return p.get("capacity_tb") * TB;
  }
  return 0;
}

double MemoryTier::peak_bw() const {
  switch (kind) {
    case MemKind::HBM: return p.get("stacks") * p.get("interface_bits") * p.get("gbps_per_pin") * 1e9 / 8.0;
    case MemKind::GDDR: return p.get("bus_width_bits") * p.get("gbps_per_pin") * 1e9 / 8.0;
    case MemKind::DRAM:
      return p.get("channels") * p.get("channel_width_bits") / 8.0 * p.get("data_rate_mtps") * 1e6;
    case MemKind::SCM: return p.get("modules") * p.get("read_gbps_per_module") * GBps;
    case MemKind::FLASH: {
      double chan = p.get("nand_channels") * p.get("channel_mtps") * 1e6;  // 8-bit NAND 总线
      double media = p.get("dies") * p.get("page_kb") * KB / (p.get("read_latency_us") * US);
      return std::min({chan, media, p.get("controller_bw_gbps") * GBps});
    }
  }
  return 0;
}

double MemoryTier::seq_read_bw() const {
  switch (kind) {
    case MemKind::SCM: return peak_bw() * 0.9;
    case MemKind::FLASH: return peak_bw() * 0.9;
    default: return peak_bw() * p.get("efficiency");
  }
}

double MemoryTier::seq_write_bw() const {
  switch (kind) {
    case MemKind::SCM: return p.get("modules") * p.get("write_gbps_per_module") * GBps * 0.9;
    case MemKind::FLASH: {
      double chan = p.get("nand_channels") * p.get("channel_mtps") * 1e6;
      double media = p.get("dies") * p.get("page_kb") * KB / (p.get("program_latency_us") * US);
      return std::min({chan, media, p.get("controller_bw_gbps") * GBps}) * 0.9;
    }
    default: return seq_read_bw();
  }
}

double MemoryTier::latency_s() const {
  switch (kind) {
    case MemKind::SCM: return p.get("read_latency_ns") * NS;
    case MemKind::FLASH: return p.get("read_latency_us") * US + 10 * US;  // + 主控/协议栈
    default: return p.get("latency_ns") * NS;
  }
}

double MemoryTier::granularity() const {
  if (kind == MemKind::FLASH) return 4096;
  return p.get("granularity_bytes");
}

double MemoryTier::pj_per_bit() const { return p.get("pj_per_bit"); }

double MemoryTier::amplification(double elem) const {
  if (elem <= 0) return 1;
  double g = granularity();
  return std::ceil(elem / g) * g / elem;
}

double MemoryTier::random_useful_bw(double elem, double outstanding) const {
  if (elem <= 0) elem = 4;
  double amp = amplification(elem);
  double reff = (kind == MemKind::FLASH) ? 0.9 : p.get("random_efficiency");
  double bw_lim = peak_bw() * reff / amp;
  // Little 定律：每个请求占用一个延迟周期；多粒度请求按流水线计
  double units = std::ceil(elem / granularity());
  double t_req = latency_s() + (units - 1) * granularity() / std::max(peak_bw(), 1.0);
  double lat_lim = std::max(outstanding, 1.0) * elem / t_req;
  double r = std::min(bw_lim, lat_lim);
  if (kind == MemKind::FLASH) {
    double die_iops = p.get("dies") / (p.get("read_latency_us") * US);
    double iops = std::min(p.get("controller_iops_m") * 1e6, die_iops);
    r = std::min(r, iops / units * elem);
  }
  return r;
}

double MemoryTier::cost_usd() const {
  double gb = capacity_bytes() / GB;
  switch (kind) {
    case MemKind::HBM: return gb * 15.0;
    case MemKind::GDDR: return gb * 5.0;
    case MemKind::DRAM: return gb * 4.0;
    case MemKind::SCM: return gb * 2.5;
    case MemKind::FLASH: return gb * 0.08 + 40;
  }
  return 0;
}

double MemoryTier::static_power_w() const {
  double gb = capacity_bytes() / GB;
  switch (kind) {
    case MemKind::HBM: return gb * 0.05;
    case MemKind::GDDR: return gb * 0.4;
    case MemKind::DRAM: return gb * 0.10;
    case MemKind::SCM: return gb * 0.03;
    case MemKind::FLASH: return 2.0 + gb / 1024.0 * 0.3;
  }
  return 0;
}

double MemoryTier::peak_power_w() const { return static_power_w() + pj_per_bit() * 1e-12 * peak_bw() * 8; }

json MemoryTier::describe_derived() const {
  json o;
  o["id"] = id;
  o["kind"] = mem_kind_name(kind);
  o["owner"] = owner;
  o["capacity_GB"] = capacity_bytes() / GB;
  o["peak_bw_GBps"] = peak_bw() / GBps;
  o["seq_read_GBps"] = seq_read_bw() / GBps;
  o["seq_write_GBps"] = seq_write_bw() / GBps;
  o["latency_ns"] = latency_s() / NS;
  o["granularity_B"] = granularity();
  o["random_64B_GBps@256outst"] = random_useful_bw(64, 256) / GBps;
  o["cost_usd"] = cost_usd();
  o["peak_power_w"] = peak_power_w();
  o["byte_addressable"] = byte_addressable();
  return o;
}

std::unique_ptr<MemoryTier> make_memory(const std::string& id, MemKind kind) {
  return std::make_unique<MemoryTier>(id, kind);
}

}  // namespace hgc
