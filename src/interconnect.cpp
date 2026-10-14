#include "hgcsim/interconnect.hpp"

#include <cmath>

#include "hgcsim/memory.hpp"

namespace hgc {

const char* link_kind_name(LinkKind k) {
  switch (k) {
    case LinkKind::PCIE: return "PCIe";
    case LinkKind::CXL: return "CXL";
    case LinkKind::DDR: return "DDR";
    case LinkKind::NVLINK: return "NVLink";
    case LinkKind::MEDIA: return "MEDIA";
  }
  return "?";
}

bool parse_link_kind(const std::string& s, LinkKind& out) {
  if (s == "PCIe" || s == "PCIE" || s == "pcie") out = LinkKind::PCIE;
  else if (s == "CXL" || s == "cxl") out = LinkKind::CXL;
  else if (s == "DDR" || s == "ddr") out = LinkKind::DDR;
  else if (s == "NVLink" || s == "NVLINK" || s == "nvlink") out = LinkKind::NVLINK;
  else return false;
  return true;
}

static double pcie_gtps(int gen) {
  static const double r[] = {0, 2.5, 5, 8, 16, 32, 64, 128};
  return (gen >= 1 && gen <= 7) ? r[gen] : 0;
}

static double pcie_encoding(int gen) {
  if (gen <= 2) return 0.8;
  if (gen <= 5) return 128.0 / 130.0;
  return 236.0 / 256.0;  // FLIT 模式（含 DLP/CRC/FEC）
}

static double cxl_gtps(double v) {
  if (v < 2.5) return 32;   // 1.1 / 2.0 (PCIe 5.0 PHY)
  if (v < 3.95) return 64;  // 3.x (PCIe 6.0 PHY)
  return 128;               // 4.0 (PCIe 7.0 PHY)
}

Link::Link(std::string id_, LinkKind k) : id(std::move(id_)), kind(k) {
  switch (kind) {
    case LinkKind::PCIE:
      p.add("gen", 4, 1, 7, "", "PCIe 代数 (Gen7=128GT/s, 2025 年发布)", true);
      p.add("lanes", 16, 1, 16, "", "通道数", true, {1, 2, 4, 8, 16});
      p.add("max_payload_bytes", 256, 128, 4096, "B", "最大 TLP 负载", true, {128, 256, 512, 1024, 2048, 4096});
      p.add("dma_efficiency", 0.92, 0.5, 1.0, "", "DMA 引擎效率");
      p.add("latency_ns", 400, 50, 2000, "ns", "单程延迟（含根复合体）");
      p.add("dma_setup_us", 5, 0.5, 50, "us", "单次 DMA 传输建立开销（驱动/API 调用）");
      p.add("zero_copy_efficiency", 0.6, 0.1, 1.0, "", "零拷贝（UVA/P2P 访存）相对 DMA 带宽效率");
      p.add("pj_per_bit", 5, 1, 20, "pJ/bit", "传输能耗");
      break;
    case LinkKind::CXL:
      p.add("version", 2.0, 1.1, 4.0, "", "CXL 版本", false, {1.1, 2.0, 3.0, 3.1, 3.2, 4.0});
      p.add("lanes", 16, 2, 16, "", "通道数 (x2 仅 CXL 4.0)", true, {2, 4, 8, 16});
      p.add("bundled_ports", 1, 1, 4, "", "捆绑端口数 (仅 CXL 4.0)", true);
      p.add("port_latency_ns", 25, 5, 100, "ns", "每个 CXL 端口穿越延迟（链路两端各一次）");
      p.add("dma_setup_us", 1.0, 0.1, 50, "us", "块传输建立开销");
      p.add("loadstore_efficiency", 0.85, 0.3, 1.0, "", "CXL.mem/.cache 访存相对链路带宽效率");
      p.add("pj_per_bit", 4, 1, 20, "pJ/bit", "传输能耗");
      break;
    case LinkKind::DDR:
      p.add("channels", 2, 1, 16, "", "所占内存通道数", true);
      p.add("data_rate_mtps", 3200, 1600, 8800, "MT/s", "总线速率");
      p.add("efficiency", 0.75, 0.2, 0.95, "", "可达带宽比例");
      p.add("latency_ns", 15, 1, 200, "ns", "总线延迟");
      p.add("pj_per_bit", 12, 3, 40, "pJ/bit", "传输能耗");
      break;
    case LinkKind::NVLINK:
      p.add("links", 12, 1, 36, "", "NVLink 链路数", true);
      p.add("gbps_per_link_per_dir", 25, 5, 100, "GB/s", "每条链路单向带宽");
      p.add("latency_ns", 500, 100, 3000, "ns", "单程延迟");
      p.add("dma_setup_us", 3, 0.5, 50, "us", "传输建立开销");
      p.add("pj_per_bit", 1.3, 0.5, 10, "pJ/bit", "传输能耗");
      break;
    case LinkKind::MEDIA:
      break;  // 参数来自所连接的 MemoryTier
  }
}

void Link::validate(std::vector<std::string>& errors) const {
  const std::string ctx = "链路 '" + id + "'";
  if (kind == LinkKind::CXL) {
    double v = p.get("version");
    if (p.geti("lanes") == 2 && v < 3.95) errors.push_back(ctx + ": 原生 x2 链路需要 CXL 4.0");
    if (p.geti("bundled_ports") > 1 && v < 3.95) errors.push_back(ctx + ": 捆绑端口 (Bundled Ports) 需要 CXL 4.0");
  }
}

double Link::raw_bw() const {
  switch (kind) {
    case LinkKind::PCIE: return pcie_gtps(p.geti("gen")) * 1e9 * p.get("lanes") / 8.0;
    case LinkKind::CXL: return cxl_gtps(p.get("version")) * 1e9 * p.get("lanes") / 8.0 * p.get("bundled_ports");
    case LinkKind::DDR: return p.get("channels") * 8.0 * p.get("data_rate_mtps") * 1e6;
    case LinkKind::NVLINK: return p.get("links") * p.get("gbps_per_link_per_dir") * GBps;
    case LinkKind::MEDIA: return media ? media->peak_bw() : 0;
  }
  return 0;
}

double Link::bw() const {
  switch (kind) {
    case LinkKind::PCIE: {
      int g = p.geti("gen");
      double mps = p.get("max_payload_bytes");
      double hdr = g <= 5 ? 24.0 : 16.0;
      return raw_bw() * pcie_encoding(g) * (mps / (mps + hdr)) * 0.95 * p.get("dma_efficiency");
    }
    case LinkKind::CXL: {
      double v = p.get("version");
      double eff = v < 2.5 ? 0.79 : 0.86;  // 68B flit vs 256B flit
      return raw_bw() * eff;
    }
    case LinkKind::DDR: return raw_bw() * p.get("efficiency");
    case LinkKind::NVLINK: return raw_bw() * 0.9;
    case LinkKind::MEDIA: return media ? media->seq_read_bw() : 0;
  }
  return 0;
}

bool Link::full_duplex() const { return kind == LinkKind::PCIE || kind == LinkKind::CXL || kind == LinkKind::NVLINK; }

double Link::latency_s() const {
  switch (kind) {
    case LinkKind::PCIE: return p.get("latency_ns") * NS;
    case LinkKind::CXL: return 2 * p.get("port_latency_ns") * NS + 5 * NS;
    case LinkKind::DDR: return p.get("latency_ns") * NS;
    case LinkKind::NVLINK: return p.get("latency_ns") * NS;
    case LinkKind::MEDIA: return media ? media->latency_s() : 0;
  }
  return 0;
}

double Link::setup_s() const {
  switch (kind) {
    case LinkKind::PCIE: return p.get("dma_setup_us") * US;
    case LinkKind::CXL: return p.get("dma_setup_us") * US;
    case LinkKind::NVLINK: return p.get("dma_setup_us") * US;
    default: return 0;
  }
}

double Link::pj_per_bit() const {
  if (kind == LinkKind::MEDIA) return media ? media->pj_per_bit() : 0;
  return p.get("pj_per_bit");
}

bool Link::load_store() const {
  switch (kind) {
    case LinkKind::PCIE: return true;  // UVA 零拷贝（效率较低）
    case LinkKind::CXL: return true;
    case LinkKind::DDR: return true;
    case LinkKind::NVLINK: return true;
    case LinkKind::MEDIA: return media && media->byte_addressable();
  }
  return false;
}

double Link::zero_copy_efficiency() const {
  switch (kind) {
    case LinkKind::PCIE: return p.get("zero_copy_efficiency");
    case LinkKind::CXL: return p.get("loadstore_efficiency");
    default: return 1.0;
  }
}

json Link::describe_derived() const {
  json o;
  o["id"] = id;
  o["kind"] = link_kind_name(kind);
  o["a"] = a;
  o["b"] = b;
  o["raw_GBps"] = raw_bw() / GBps;
  o["effective_GBps"] = bw() / GBps;
  o["latency_ns"] = latency_s() / NS;
  o["full_duplex"] = full_duplex();
  o["load_store"] = load_store();
  if (kind == LinkKind::CXL) o["cxl_version"] = p.get("version");
  if (kind == LinkKind::PCIE) o["pcie_gen"] = p.geti("gen");
  return o;
}

}  // namespace hgc
