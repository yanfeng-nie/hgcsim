// =============================================================================
// system.hpp : 计算机系统拓扑（设备 + 存储 + 互连 + 交换机）与路由
//
// 拓扑为一棵以主机 CPU（兼根复合体）为根的树，可附加对等链路（NVLink）。
// 每个存储层级作为独立节点，经“介质链路”(MEDIA) 连接到其所属设备。
// 传输路径由路由器自动计算（最宽路径优先，其次最低延迟），并遵守以下规则：
//   * CXL 交换机需要 CXL ≥ 2.0；交换机级联（多级交换）需要 CXL ≥ 3.0
//   * 同一 CXL 交换机下设备间直接 P2P 需要 CXL ≥ 3.0，否则须绕行主机
//   * PCIe 交换机下的 P2P 由交换机参数 p2p_enabled 决定
//   * DDR 总线挂载的设备（NMP / SCM DIMM）与主机 DRAM 共享内存通道数
// =============================================================================
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hgcsim/device.hpp"
#include "hgcsim/interconnect.hpp"
#include "hgcsim/memory.hpp"

namespace hgc {

struct Switch {
  std::string id;
  std::string type;  // "PCIe" | "CXL"
  ParamSet p;
  double latency_s() const { return p.get("latency_ns") * NS; }
};

struct Path {
  bool ok = false;
  std::string error;
  std::vector<int> links;    // 链路下标
  std::vector<int> dirs;     // 0 = a->b (上行), 1 = b->a (下行)
  std::vector<std::string> nodes;
  double latency_s = 0;
  double bottleneck_bw = 0;
  double setup_s = 0;
  bool load_store = true;
  double zero_copy_eff = 1.0;
  double pj_per_bit = 0;     // 不含两端介质
  bool has_media_src = false, has_media_dst = false;
};

class System {
 public:
  std::string name;
  std::string description;
  std::vector<std::unique_ptr<ComputeDevice>> devices;  // devices[0] 为主机 CPU
  std::vector<std::unique_ptr<MemoryTier>> memories;
  std::vector<std::unique_ptr<Link>> links;
  std::vector<std::unique_ptr<Switch>> switches;
  bool budget_enabled = false;
  double power_budget_w = 0, cost_budget_usd = 0;
  json source;  // 原始配置

  static std::unique_ptr<System> from_json(const json& j, std::vector<std::string>& errors);

  ComputeDevice* host() const { return devices.empty() ? nullptr : devices[0].get(); }
  ComputeDevice* device(const std::string& id) const;
  MemoryTier* memory(const std::string& id) const;
  Switch* sw(const std::string& id) const;
  bool is_node(const std::string& id) const;

  // 路由：src / dst 可以是存储节点或设备节点
  const Path& route(const std::string& src, const std::string& dst) const;

  double total_cost() const;
  double total_tdp() const;
  double total_idle_w() const;
  json describe() const;
  void reset_stats();

 private:
  std::map<std::string, std::vector<int>> adj_;
  std::map<std::string, std::string> node_kind_;  // device | memory | switch
  mutable std::map<std::pair<std::string, std::string>, Path> route_cache_;
  void build_index();
  Path compute_route(const std::string& src, const std::string& dst) const;
  double node_latency(const std::string& n) const;
};

json catalog();  // 所有组件类型的参数说明（供前端生成配置界面）

}  // namespace hgc
