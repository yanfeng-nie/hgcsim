// =============================================================================
// interconnect.hpp : 互连抽象 —— PCIe / CXL / DDR 总线 / NVLink / 介质链路
//
// PCIe 有效带宽 = 每通道速率(GT/s) × 通道数 / 8 × 编码效率 × TLP 效率 × DLLP 效率 × DMA 效率
//   编码: Gen1-2 8b/10b, Gen3-5 128b/130b, Gen6-7 FLIT(236/256, 含 FEC/CRC)
//   TLP 效率 = MaxPayload / (MaxPayload + 头开销)  (Gen≤5 头开销 24B，Gen6+ 16B)
// CXL 有效带宽 = PHY 速率 × 通道数 / 8 × flit 效率 × 捆绑端口数
//   CXL 1.1/2.0: 32 GT/s, 68B flit；CXL 3.x: 64 GT/s, 256B flit；CXL 4.0: 128 GT/s
//   特性: 2.0 起支持交换/内存池化；3.0 起支持多级交换、内存共享、设备间直连 P2P；
//         4.0 支持捆绑端口 (Bundled Ports) 与原生 x2 链路
// =============================================================================
#pragma once
#include <string>
#include <vector>

#include "hgcsim/params.hpp"

namespace hgc {

class MemoryTier;

enum class LinkKind { PCIE, CXL, DDR, NVLINK, MEDIA };

const char* link_kind_name(LinkKind k);
bool parse_link_kind(const std::string& s, LinkKind& out);

class Link {
 public:
  Link(std::string id, LinkKind kind);

  std::string id;
  LinkKind kind;
  ParamSet p;
  std::string a;  // 下游端（设备 / 存储节点）
  std::string b;  // 上游端（父节点：cpu0 / 交换机 / 所属设备）
  MemoryTier* media = nullptr;  // MEDIA 链路：所连接的存储介质

  void validate(std::vector<std::string>& errors) const;

  double raw_bw() const;       // 物理层原始带宽 (bytes/s, 单向)
  double bw() const;           // 协议后可达带宽 (bytes/s, 单向)
  bool full_duplex() const;    // 全双工：两个方向带宽独立
  double latency_s() const;    // 单程延迟
  double setup_s() const;      // 一次 DMA 传输的建立开销
  double pj_per_bit() const;
  bool load_store() const;     // 是否支持访存语义（零拷贝 / CXL.mem / CXL.cache）
  double zero_copy_efficiency() const;
  double cxl_version() const { return kind == LinkKind::CXL ? p.get("version") : 0; }

  json describe_derived() const;

  // 运行时统计
  double bytes_fwd = 0;  // a -> b
  double bytes_rev = 0;  // b -> a
  void reset_stats() { bytes_fwd = bytes_rev = 0; }
};

}  // namespace hgc
